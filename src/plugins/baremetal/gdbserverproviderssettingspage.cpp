/****************************************************************************
**
** Copyright (C) 2016 Denis Shienkov <denis.shienkov@gmail.com>
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "baremetalconstants.h"

#include "gdbserverprovider.h"
#include "gdbserverprovidermanager.h"
#include "gdbserverproviderssettingspage.h"

#include <coreplugin/icore.h>
#include <extensionsystem/pluginmanager.h>
#include <projectexplorer/projectexplorerconstants.h>

#include <utils/algorithm.h>
#include <utils/detailswidget.h>
#include <utils/qtcassert.h>

#include <QAction>
#include <QApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpacerItem>
#include <QTextStream>
#include <QTreeView>
#include <QVBoxLayout>

using namespace Utils;

namespace BareMetal {
namespace Internal {

// GdbServerProviderNode

class GdbServerProviderNode final : public TreeItem
{
public:
    explicit GdbServerProviderNode(GdbServerProvider *provider, bool changed = false)
        : provider(provider), changed(changed)
    {
    }

    QVariant data(int column, int role) const final
    {
        if (role == Qt::FontRole) {
            QFont f = QApplication::font();
            if (changed)
                f.setBold(true);
            return f;
        }

        if (role == Qt::DisplayRole) {
            return column == 0 ? provider->displayName() : provider->typeDisplayName();
        }

        // FIXME: Need to handle ToolTipRole role?
        return {};
    }

    GdbServerProvider *provider = nullptr;
    GdbServerProviderConfigWidget *widget = nullptr;
    bool changed = false;
};

// GdbServerProviderModel

GdbServerProviderModel::GdbServerProviderModel()
{
    setHeader({tr("Name"), tr("Type")});

    const GdbServerProviderManager *manager = GdbServerProviderManager::instance();

    connect(manager, &GdbServerProviderManager::providerAdded,
            this, &GdbServerProviderModel::addProvider);
    connect(manager, &GdbServerProviderManager::providerRemoved,
            this, &GdbServerProviderModel::removeProvider);

    for (GdbServerProvider *p : GdbServerProviderManager::providers())
        addProvider(p);
}

GdbServerProvider *GdbServerProviderModel::provider(const QModelIndex &index) const
{
    if (const GdbServerProviderNode *node = nodeForIndex(index))
        return node->provider;

    return nullptr;
}

GdbServerProviderNode *GdbServerProviderModel::nodeForIndex(const QModelIndex &index) const
{
    if (!index.isValid())
        return nullptr;

    return static_cast<GdbServerProviderNode *>(itemForIndex(index));
}

void GdbServerProviderModel::apply()
{
    // Remove unused providers
    for (GdbServerProvider *provider : qAsConst(m_providersToRemove))
        GdbServerProviderManager::deregisterProvider(provider);
    QTC_ASSERT(m_providersToRemove.isEmpty(), m_providersToRemove.clear());

    // Update providers
    for (TreeItem *item : *rootItem()) {
        const auto n = static_cast<GdbServerProviderNode *>(item);
        if (!n->changed)
            continue;

        QTC_CHECK(n->provider);
        if (n->widget)
            n->widget->apply();

        n->changed = false;
        n->update();
    }

    // Add new (and already updated) providers
    QStringList skippedProviders;
    for (GdbServerProvider *provider: qAsConst(m_providersToAdd)) {
        if (!GdbServerProviderManager::registerProvider(provider))
            skippedProviders << provider->displayName();
    }

    m_providersToAdd.clear();

    if (!skippedProviders.isEmpty()) {
        QMessageBox::warning(Core::ICore::dialogParent(),
                             tr("Duplicate Providers Detected"),
                             tr("The following providers were already configured:<br>"
                                "&nbsp;%1<br>"
                                "They were not configured again.")
                             .arg(skippedProviders.join(QLatin1String(",<br>&nbsp;"))));
    }
}

GdbServerProviderNode *GdbServerProviderModel::findNode(const GdbServerProvider *provider) const
{
    auto test = [provider](TreeItem *item) {
        return static_cast<GdbServerProviderNode *>(item)->provider == provider;
    };

    return static_cast<GdbServerProviderNode *>(Utils::findOrDefault(*rootItem(), test));
}

QModelIndex GdbServerProviderModel::indexForProvider(GdbServerProvider *provider) const
{
    const GdbServerProviderNode *n = findNode(provider);
    return n ? indexForItem(n) : QModelIndex();
}

void GdbServerProviderModel::markForRemoval(GdbServerProvider *provider)
{
    GdbServerProviderNode *n = findNode(provider);
    QTC_ASSERT(n, return);
    destroyItem(n);

    if (m_providersToAdd.contains(provider)) {
        m_providersToAdd.removeOne(provider);
        delete provider;
    } else {
        m_providersToRemove.append(provider);
    }
}

void GdbServerProviderModel::markForAddition(GdbServerProvider *provider)
{
    GdbServerProviderNode *n = createNode(provider, true);
    rootItem()->appendChild(n);
    m_providersToAdd.append(provider);
}

GdbServerProviderNode *GdbServerProviderModel::createNode(
        GdbServerProvider *provider, bool changed)
{
    const auto node = new GdbServerProviderNode(provider, changed);
    node->widget = provider->configurationWidget();
    connect(node->widget, &GdbServerProviderConfigWidget::dirty, this, [node] {
        node->changed = true;
        node->update();
    });
    return node;
}

void GdbServerProviderModel::addProvider(GdbServerProvider *provider)
{
    if (findNode(provider))
        m_providersToAdd.removeOne(provider);
    else
        rootItem()->appendChild(createNode(provider, false));

    emit providerStateChanged();
}

void GdbServerProviderModel::removeProvider(GdbServerProvider *provider)
{
    m_providersToRemove.removeAll(provider);
    if (GdbServerProviderNode *n = findNode(provider))
        destroyItem(n);

    emit providerStateChanged();
}

// GdbServerProvidersSettingsWidget

class GdbServerProvidersSettingsWidget final : public QWidget
{
    Q_DECLARE_TR_FUNCTIONS(BareMetal::Internal::GdbServerProvidersSettingsPage)

public:
    explicit GdbServerProvidersSettingsWidget(GdbServerProvidersSettingsPage *page);

    void providerSelectionChanged();
    void removeProvider();
    void updateState();

    void createProvider(GdbServerProviderFactory *f);
    QModelIndex currentIndex() const;

public:
    GdbServerProvidersSettingsPage *m_page = nullptr;
    GdbServerProviderModel m_model;
    QItemSelectionModel *m_selectionModel = nullptr;
    QTreeView *m_providerView = nullptr;
    Utils::DetailsWidget *m_container = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_cloneButton = nullptr;
    QPushButton *m_delButton = nullptr;
};

GdbServerProvidersSettingsWidget::GdbServerProvidersSettingsWidget
        (GdbServerProvidersSettingsPage *page)
    : m_page(page)
{
    m_providerView = new QTreeView(this);
    m_providerView->setUniformRowHeights(true);
    m_providerView->header()->setStretchLastSection(false);

    m_addButton = new QPushButton(tr("Add"), this);
    m_cloneButton = new QPushButton(tr("Clone"), this);
    m_delButton = new QPushButton(tr("Remove"), this);

    m_container = new Utils::DetailsWidget(this);
    m_container->setState(Utils::DetailsWidget::NoSummary);
    m_container->setMinimumWidth(500);
    m_container->setVisible(false);

    const auto buttonLayout = new QHBoxLayout;
    buttonLayout->setSpacing(6);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->addWidget(m_addButton);
    buttonLayout->addWidget(m_cloneButton);
    buttonLayout->addWidget(m_delButton);
    const auto spacerItem = new QSpacerItem(40, 10, QSizePolicy::Expanding, QSizePolicy::Minimum);
    buttonLayout->addItem(spacerItem);

    const auto verticalLayout = new QVBoxLayout;
    verticalLayout->addWidget(m_providerView);
    verticalLayout->addLayout(buttonLayout);

    const auto horizontalLayout = new QHBoxLayout;
    horizontalLayout->addLayout(verticalLayout);
    horizontalLayout->addWidget(m_container);

    const auto groupBox = new QGroupBox(tr("GDB Server Providers"), this);
    groupBox->setLayout(horizontalLayout);

    const auto topLayout = new QVBoxLayout(this);
    topLayout->addWidget(groupBox);

    connect(&m_model, &GdbServerProviderModel::providerStateChanged,
            this, &GdbServerProvidersSettingsWidget::updateState);

    m_providerView->setModel(&m_model);

    const auto headerView = m_providerView->header();
    headerView->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    headerView->setSectionResizeMode(1, QHeaderView::Stretch);
    m_providerView->expandAll();

    m_selectionModel = m_providerView->selectionModel();

    connect(m_selectionModel, &QItemSelectionModel::selectionChanged,
            this, &GdbServerProvidersSettingsWidget::providerSelectionChanged);

    connect(GdbServerProviderManager::instance(), &GdbServerProviderManager::providersChanged,
            this, &GdbServerProvidersSettingsWidget::providerSelectionChanged);

    // Set up add menu:
    const auto addMenu = new QMenu(m_addButton);

    for (const auto f : GdbServerProviderManager::factories()) {
        const auto action = new QAction(addMenu);
        action->setText(f->displayName());
        connect(action, &QAction::triggered, this, [this, f] { createProvider(f); });
        addMenu->addAction(action);
    }

    connect(m_cloneButton, &QAbstractButton::clicked, this, [this] { createProvider(nullptr); });

    m_addButton->setMenu(addMenu);

    connect(m_delButton, &QPushButton::clicked,
            this, &GdbServerProvidersSettingsWidget::removeProvider);

    updateState();
}

void GdbServerProvidersSettingsWidget::providerSelectionChanged()
{
    if (!m_container)
        return;
    const QModelIndex current = currentIndex();
    QWidget *w = m_container->takeWidget(); // Prevent deletion.
    if (w)
        w->setVisible(false);

    const GdbServerProviderNode *node = m_model.nodeForIndex(current);
    w = node ? node->widget : nullptr;
    m_container->setWidget(w);
    m_container->setVisible(w != nullptr);
    updateState();
}

void GdbServerProvidersSettingsWidget::createProvider(GdbServerProviderFactory *f)
{
    GdbServerProvider *provider = nullptr;
    if (!f) {
        const GdbServerProvider *old = m_model.provider(currentIndex());
        if (!old)
            return;
        provider = old->clone();
    } else {
        provider = f->create();
    }

    if (!provider)
        return;

    m_model.markForAddition(provider);

    m_selectionModel->select(m_model.indexForProvider(provider),
                             QItemSelectionModel::Clear
                             | QItemSelectionModel::SelectCurrent
                             | QItemSelectionModel::Rows);
}

void GdbServerProvidersSettingsWidget::removeProvider()
{
    if (GdbServerProvider *p = m_model.provider(currentIndex()))
        m_model.markForRemoval(p);
}

void GdbServerProvidersSettingsWidget::updateState()
{
    if (!m_cloneButton)
        return;

    bool canCopy = false;
    bool canDelete = false;
    if (const GdbServerProvider *p = m_model.provider(currentIndex())) {
        canCopy = p->isValid();
        canDelete = true;
    }

    m_cloneButton->setEnabled(canCopy);
    m_delButton->setEnabled(canDelete);
}

QModelIndex GdbServerProvidersSettingsWidget::currentIndex() const
{
    if (!m_selectionModel)
        return {};

    const QModelIndexList rows = m_selectionModel->selectedRows();
    if (rows.count() != 1)
        return {};
    return rows.at(0);
}


GdbServerProvidersSettingsPage::GdbServerProvidersSettingsPage(QObject *parent)
    : Core::IOptionsPage(parent)
{
    setId(Constants::GDB_PROVIDERS_SETTINGS_ID);
    setDisplayName(tr("Bare Metal"));
    setCategory(ProjectExplorer::Constants::DEVICE_SETTINGS_CATEGORY);
}

QWidget *GdbServerProvidersSettingsPage::widget()
{
    if (!m_configWidget)
        m_configWidget = new GdbServerProvidersSettingsWidget(this);
     return m_configWidget;
}

void GdbServerProvidersSettingsPage::apply()
{
    if (m_configWidget)
        m_configWidget->m_model.apply();
}

void GdbServerProvidersSettingsPage::finish()
{
    if (m_configWidget)
        disconnect(GdbServerProviderManager::instance(), &GdbServerProviderManager::providersChanged,
                   m_configWidget, &GdbServerProvidersSettingsWidget::providerSelectionChanged);

    delete m_configWidget;
    m_configWidget = nullptr;
}

} // namespace Internal
} // namespace BareMetal
