#include <string>
#include <vector>
#include <iostream>

#include <QFileDialog>
#include <QToolButton>
#include <QIcon>
#include <QLabel>
#include <QGraphicsEffect>
#include <QDebug>
#include <QPushButton>
#include <QDockWidget>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPluginLoader>
#include <QComboBox>
#include <QToolBar>
#include <QStyle>
#include <QActionGroup>

#include "pathologyworkstation.h"
#include "PathologyViewer.h"
#include "FilterDockWidget.h"
#include "interfaces/interfaces.h"
#include "WSITileGraphicsItemCache.h"
#include "io/multiresolutionimageinterface/MultiResolutionImageReader.h"
#include "io/multiresolutionimageinterface/MultiResolutionImage.h"

using namespace std;

PathologyWorkstation::PathologyWorkstation(QWidget *parent) :
    QMainWindow(parent),
    _img(NULL),
    _cacheMaxByteSize(1000*512*512*3),
    _filterDock(NULL),
    _filters(new std::vector<std::shared_ptr<ImageFilterPluginInterface> >())
{
  setupUi();
  retranslateUi();
  connect(actionOpen, SIGNAL(triggered(bool)), this, SLOT(on_actionOpen_triggered()));
  connect(actionClose, SIGNAL(triggered(bool)), this, SLOT(on_actionClose_triggered()));

  this->initializeDocks();
  PathologyViewer* view = this->findChild<PathologyViewer*>("pathologyView");
  this->loadPlugins();
  view->setActiveTool("pan");
  view->setCacheSize(_cacheMaxByteSize);
  QAction* pan = mainToolBar->actions().at(3);
  if (pan) {
    pan->setChecked(true);
  }
}

void PathologyWorkstation::loadPlugins() {
  PathologyViewer* viewer = this->findChild<PathologyViewer*>("pathologyView");
  _pluginsDir = QDir(qApp->applicationDirPath());
  if (_pluginsDir.cd("plugins")) {
    if (_pluginsDir.cd("tools")) {
      foreach(QString fileName, _pluginsDir.entryList(QDir::Files)) {
        if (fileName.toLower().endsWith(".dll")) {
          QPluginLoader loader(_pluginsDir.absoluteFilePath(fileName));
          QObject *plugin = loader.instance();
          if (plugin) {
            ToolPluginInterface* tool = qobject_cast<ToolPluginInterface*>(plugin);
            if (tool) {
              tool->setViewer(viewer);
              QAction* toolAction = tool->getToolButton();
              connect(toolAction, SIGNAL(triggered(bool)), viewer, SLOT(changeActiveTool()));
              _toolPluginFileNames.push_back(fileName.toStdString());
              viewer->addTool(tool);
              QToolBar* mainToolBar = this->findChild<QToolBar *>("mainToolBar");
              toolAction->setCheckable(true);
              _toolActions->addAction(toolAction);
              mainToolBar->addAction(toolAction);
            }
          }
        }
      }
      _pluginsDir.cdUp();
    }
    if (_pluginsDir.cd("workstationextension")) {
      foreach(QString fileName, _pluginsDir.entryList(QDir::Files)) {
        if (fileName.toLower().endsWith(".dll")) {
          QPluginLoader loader(_pluginsDir.absoluteFilePath(fileName));
          QObject *plugin = loader.instance();
          if (plugin) {
            std::unique_ptr<WorkstationExtensionPluginInterface> extension(qobject_cast<WorkstationExtensionPluginInterface*>(plugin));
            if (extension) {
              _extensionPluginFileNames.push_back(fileName.toStdString());
              connect(this, SIGNAL(newImageLoaded(MultiResolutionImage*, std::string)), &*extension, SLOT(onNewImageLoaded(MultiResolutionImage*, std::string)));
              connect(this, SIGNAL(imageClosed()), &*extension, SLOT(onImageClosed()));
              extension->initialize(viewer);
              if (extension->getToolBar()) {
                this->addToolBar(extension->getToolBar());
              }
              if (extension->getDockWidget()) {
                this->addDockWidget(Qt::LeftDockWidgetArea, extension->getDockWidget());
              }
              if (extension->getMenu()) {
                this->menuBar->addMenu(extension->getMenu());
              }
              std::vector<ToolPluginInterface*> tools = extension->getTools();
              if (!tools.empty()) {
                mainToolBar->addSeparator();
                for (unsigned int i = 0; i < tools.size(); ++i) {
                  QAction* toolAction = tools[i]->getToolButton();
                  connect(toolAction, SIGNAL(triggered(bool)), viewer, SLOT(changeActiveTool()));
                  viewer->addTool(tools[i]);
                  mainToolBar->addAction(toolAction);
                  toolAction->setCheckable(true);
                  _toolActions->addAction(toolAction);
                }
              }
              _extensions.push_back(std::move(extension));
            }
          }
        }
      }
      _pluginsDir.cdUp();
    }
    if (_pluginsDir.cd("filters")) {
      QListWidget* availableFilters = _filterDock->findChild<QListWidget*>("filterListWidget");
      foreach(QString fileName, _pluginsDir.entryList(QDir::Files)) {
        if (fileName.toLower().endsWith(".dll")) {
          QPluginLoader loader(_pluginsDir.absoluteFilePath(fileName));
          QObject *plugin = loader.instance();
          if (plugin) {
            std::shared_ptr<ImageFilterPluginInterface> filter(qobject_cast<ImageFilterPluginInterface *>(plugin));
            if (filter) {
              _filterPluginFileNames.push_back(fileName.toStdString());
              if (_filterDock) {
                QListWidgetItem* filterItem = new QListWidgetItem(filter->icon(), filter->name());
                filterItem->setData(Qt::ItemDataRole::UserRole, QVariant::fromValue(filter));      
                availableFilters->addItem(filterItem);
                filterItem->setHidden(true);
              }
            }
          }
        }
      }
    }
  }
}

void PathologyWorkstation::initializeDocks()
{
  _filterDock = new FilterDockWidget(this);
  _filterDock->setEnabled(false);
  _filterDock->setVisible(true);
  _filterDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
  connect(this, SIGNAL(newImageLoaded(MultiResolutionImage*, std::string)), _filterDock, SLOT(onNewImageLoaded(MultiResolutionImage*)));
  connect(this, SIGNAL(imageClosed()), _filterDock, SLOT(onImageClosed()));
  connect(_filterDock, SIGNAL(requestFilterResultUpdate()), this, SLOT(onFilterResultUpdateRequested()));
  connect(_filterDock, SIGNAL(requestFilterResultClear()), this, SLOT(onFilterResultClearRequested()));
  connect(_filterDock, SIGNAL(changeAutoUpdateStatus(bool)), this, SLOT(onAutoUpdateStatusChanged(bool)));
  addDockWidget(Qt::LeftDockWidgetArea, _filterDock);
}

PathologyWorkstation::~PathologyWorkstation()
{
  this->findChild<PathologyViewer*>("pathologyView")->close();
  delete _img;
}

void PathologyWorkstation::onFilterResultClearRequested() {
  PathologyViewer* view = this->findChild<PathologyViewer*>("pathologyView");
  if (view) {
    view->clearFilterResult();
  }
}

void PathologyWorkstation::onFilterResultUpdateRequested() {
  PathologyViewer* view = this->findChild<PathologyViewer*>("pathologyView");
  if (view) {
    view->updateFilterResult();
  }
}

void PathologyWorkstation::onAutoUpdateStatusChanged(bool autoUpdate) {
  PathologyViewer* view = this->findChild<PathologyViewer*>("pathologyView");
  if (view) {
    view->setAutoUpdate(autoUpdate);
    if (autoUpdate) {
      view->updateFilterResult();
    }
  }
}

void PathologyWorkstation::on_actionClose_triggered()
{
    emit imageClosed();
    if (_img) {
		  PathologyViewer* view = this->findChild<PathologyViewer*>("pathologyView");
      disconnect(_filterDock, SIGNAL(changeCurrentFilter(std::shared_ptr<ImageFilterPluginInterface>)), view, SLOT(onChangeCurrentFilter(std::shared_ptr<ImageFilterPluginInterface>)));
      _filterDock->onImageClosed();
      _filterDock->setEnabled(false);

		  view->close();
		  delete _img;
		  _img = NULL;
		  statusBar->showMessage("Closed file!", 5);
    }
}

void PathologyWorkstation::on_actionOpen_triggered()
{
    if (_img) {
      on_actionClose_triggered();
    }
    QString fileName = QFileDialog::getOpenFileName(this, tr("Open File"), "D:\\Demo",  tr("Slide files (*.lif;*.svs;*.mrxs;*.tif;*.tiff)"));
    if (!fileName.isEmpty()) {
      std::string fn = fileName.toStdString();
      MultiResolutionImageReader imgReader;
      _img = imgReader.open(fn);
      if (_img) {
          if (_img->valid()) {
            vector<unsigned long long> dimensions = _img->getLevelDimensions(_img->getNumberOfLevels()-1);
            PathologyViewer* view = this->findChild<PathologyViewer*>("pathologyView");
            view->initialize(_img);
            _filterDock->setEnabled(true);
            connect(_filterDock, SIGNAL(changeCurrentFilter(std::shared_ptr<ImageFilterPluginInterface>)), view, SLOT(onChangeCurrentFilter(std::shared_ptr<ImageFilterPluginInterface>)));
            emit newImageLoaded(_img, fn);
          } else {
            statusBar->showMessage("Unsupported file type version");
          }
      } else {
          statusBar->showMessage("Invalid file type");
      }
    }
}

void PathologyWorkstation::setCacheSize(const unsigned long long& cacheMaxByteSize) {
  PathologyViewer* view = this->findChild<PathologyViewer*>("pathologyView");
  if (view) {
    view->setCacheSize(_cacheMaxByteSize);
  }
}
    
unsigned long long PathologyWorkstation::getCacheSize() const {
  PathologyViewer* view = this->findChild<PathologyViewer*>("pathologyView");
  if (view) {
    return view->getCacheSize();
  }
}

void PathologyWorkstation::setupUi()
{
  if (this->objectName().isEmpty()) {
      this->setObjectName(QStringLiteral("ASAP"));
  }
  this->resize(1037, 786);
  QSizePolicy sizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  sizePolicy.setHorizontalStretch(0);
  sizePolicy.setVerticalStretch(0);
  sizePolicy.setHeightForWidth(this->sizePolicy().hasHeightForWidth());
  this->setSizePolicy(sizePolicy);
  actionOpen = new QAction(this);
  actionOpen->setObjectName(QStringLiteral("actionOpen"));
  actionClose = new QAction(this);
  actionClose->setObjectName(QStringLiteral("actionClose"));
  actionOpen->setIcon(QIcon(QPixmap(":/icons/open.png")));
  actionClose->setIcon(QIcon(QPixmap(":/icons/close.png")));
  centralWidget = new QWidget(this);
  centralWidget->setObjectName(QStringLiteral("centralWidget"));
  sizePolicy.setHeightForWidth(centralWidget->sizePolicy().hasHeightForWidth());
  centralWidget->setSizePolicy(sizePolicy);
  centralWidget->setLocale(QLocale(QLocale::English, QLocale::UnitedStates));
  horizontalLayout_2 = new QHBoxLayout(centralWidget);
  horizontalLayout_2->setSpacing(6);
  horizontalLayout_2->setContentsMargins(0, 0, 0, 0);
  horizontalLayout_2->setObjectName(QStringLiteral("horizontalLayout_2"));
  pathologyView = new PathologyViewer(centralWidget);
  pathologyView->setObjectName(QStringLiteral("pathologyView"));

  horizontalLayout_2->addWidget(pathologyView);

  this->setCentralWidget(centralWidget);
  menuBar = new QMenuBar(this);
  menuBar->setObjectName(QStringLiteral("menuBar"));
  menuBar->setGeometry(QRect(0, 0, 1037, 21));
  menuFile = new QMenu(menuBar);
  menuFile->setObjectName(QStringLiteral("menuFile"));
  menuEdit = new QMenu(menuBar);
  menuEdit->setObjectName(QStringLiteral("menuEdit"));
  menuView = new QMenu(menuBar);
  menuView->setObjectName(QStringLiteral("menuView"));
  this->setMenuBar(menuBar);
  mainToolBar = new QToolBar(this);
  mainToolBar->setObjectName(QStringLiteral("mainToolBar"));
  mainToolBar->addAction(actionOpen);
  mainToolBar->addAction(actionClose);
  mainToolBar->addSeparator();
  this->addToolBar(Qt::TopToolBarArea, mainToolBar);
  _toolActions = new QActionGroup(this);
  statusBar = new QStatusBar(this);
  statusBar->setObjectName(QStringLiteral("statusBar"));
  this->setStatusBar(statusBar);

  menuBar->addAction(menuFile->menuAction());
  menuBar->addAction(menuEdit->menuAction());
  menuBar->addAction(menuView->menuAction());
  menuFile->addAction(actionOpen);
  menuFile->addAction(actionClose);
}

void PathologyWorkstation::retranslateUi()
{
  this->setWindowTitle(QApplication::translate("PathologyWorkstation", "ASAP", 0));
  actionOpen->setText(QApplication::translate("PathologyWorkstation", "Open", 0));
  actionOpen->setIconText(QApplication::translate("PathologyWorkstation", "Open", 0));
  actionOpen->setShortcut(QApplication::translate("PathologyWorkstation", "Ctrl+O", 0));
  actionClose->setText(QApplication::translate("PathologyWorkstation", "Close", 0));
  actionClose->setShortcut(QApplication::translate("PathologyWorkstation", "Ctrl+C", 0));
  actionClose->setIconText(QApplication::translate("PathologyWorkstation", "Close", 0));
  menuFile->setTitle(QApplication::translate("PathologyWorkstation", "File", 0));
  menuEdit->setTitle(QApplication::translate("PathologyWorkstation", "Edit", 0));
  menuView->setTitle(QApplication::translate("PathologyWorkstation", "View", 0));
} 