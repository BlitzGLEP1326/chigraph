#include "mainwindow.hpp"

#include <KActionCollection>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KMessageBox>
#include <KStandardAction>

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QDockWidget>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSplitter>
#include <QTextStream>

#include <chig/CModule.hpp>
#include <chig/Config.hpp>
#include <chig/Context.hpp>
#include <chig/GraphFunction.hpp>
#include <chig/JsonModule.hpp>
#include <chig/LangModule.hpp>
#include <chig/json.hpp>

#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/Support/raw_ostream.h>

#include <fstream>

#include <KSharedConfig>
#include "chigraphnodemodel.hpp"

MainWindow::MainWindow(QWidget* parent) : KXmlGuiWindow(parent)
{
	Q_INIT_RESOURCE(chiggui);

	// set icon
	setWindowIcon(QIcon(":/icons/chigraphsmall.png"));

	mChigContext = std::make_unique<chig::Context>();

	// setup functions pane
	QDockWidget* docker = new QDockWidget(i18n("Functions"), this);
	docker->setObjectName("Functions");
	auto functionsPane = new FunctionsPane(this, this);
	docker->setWidget(functionsPane);
	addDockWidget(Qt::LeftDockWidgetArea, docker);
	connect(
		functionsPane, &FunctionsPane::functionSelected, this, &MainWindow::newFunctionSelected);
	connect(this, &MainWindow::newFunctionCreated, functionsPane,
		[functionsPane](
			chig::GraphFunction* func) { functionsPane->updateModule(&func->module()); });

	// setup module browser
	docker = new QDockWidget(i18n("Modules"), this);
	docker->setObjectName("Modules");
	auto moduleBrowser = new ModuleBrowser(this);
	docker->setWidget(moduleBrowser);
	addDockWidget(Qt::LeftDockWidgetArea, docker);
	connect(this, &MainWindow::workspaceOpened, moduleBrowser, &ModuleBrowser::loadWorkspace);
	connect(moduleBrowser, &ModuleBrowser::moduleSelected, this, &MainWindow::openModule);
	connect(this, &MainWindow::newModuleCreated, moduleBrowser,
		[moduleBrowser](chig::JsonModule* mod) { moduleBrowser->loadWorkspace(mod->context()); });

	mFunctionTabs = new QTabWidget(this);
	mFunctionTabs->setMovable(true);
	mFunctionTabs->setTabsClosable(true);
	connect(mFunctionTabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);

	setCentralWidget(mFunctionTabs);

	docker = new QDockWidget(i18n("Output"), this);
	docker->setObjectName("Output");
	auto outputView = new OutputView;
	docker->setWidget(outputView);
	addDockWidget(Qt::BottomDockWidgetArea, docker);
	connect(this, &MainWindow::runStarted, outputView, &OutputView::setProcess);

	docker = new QDockWidget(i18n("Function Details"), this);
	docker->setObjectName("Function Details");
	auto functionDetails = new FunctionDetails;
	docker->setWidget(functionDetails);
	addDockWidget(Qt::RightDockWidgetArea, docker);
	connect(this, &MainWindow::functionOpened, functionDetails, &FunctionDetails::loadFunction);

	docker = new QDockWidget(i18n("Module Dependencies"), this);
	docker->setObjectName("Module Dependencies");
	auto mModuleDeps = new ModuleDependencies;
	docker->setWidget(mModuleDeps);
	addDockWidget(Qt::RightDockWidgetArea, docker);
	connect(this, &MainWindow::moduleOpened, mModuleDeps, &ModuleDependencies::setModule);

	/// Setup actions
	auto actColl = this->KXmlGuiWindow::actionCollection();

	KStandardAction::quit(qApp, SLOT(quit()), actColl);

	QAction* openAction = actColl->addAction(KStandardAction::Open, QStringLiteral("open"));
	openAction->setWhatsThis(QStringLiteral("Open a chigraph workspace"));
	connect(openAction, &QAction::triggered, this, &MainWindow::openWorkspaceDialog);

	mOpenRecentAction = KStandardAction::openRecent(this, &MainWindow::openWorkspace, nullptr);
	actColl->addAction(QStringLiteral("open-recent"), mOpenRecentAction);

	mOpenRecentAction->setToolBarMode(KSelectAction::MenuMode);
	mOpenRecentAction->setToolButtonPopupMode(QToolButton::DelayedPopup);
	mOpenRecentAction->setIconText(i18nc("action, to open an archive", "Open"));
	mOpenRecentAction->setToolTip(i18n("Open an archive"));
	mOpenRecentAction->loadEntries(KSharedConfig::openConfig()->group("Recent Files"));

	auto newAction = actColl->addAction(KStandardAction::New, QStringLiteral("new"));
	newAction->setWhatsThis(QStringLiteral("Create a new chigraph module"));

	auto saveAction = actColl->addAction(KStandardAction::Save, QStringLiteral("save"));
	saveAction->setWhatsThis(QStringLiteral("Save the chigraph module"));
	connect(saveAction, &QAction::triggered, this, &MainWindow::save);

	auto runAction = new QAction;
	runAction->setText(i18n("&Run"));
	runAction->setIcon(QIcon::fromTheme("system-run"));
	actColl->setDefaultShortcut(runAction, Qt::CTRL + Qt::Key_R);
	actColl->addAction(QStringLiteral("run"), runAction);
	connect(runAction, &QAction::triggered, this, &MainWindow::run);

	auto cancelAction = new QAction;
	cancelAction->setText(i18n("Cancel"));
	cancelAction->setIcon(QIcon::fromTheme("process-stop"));
	actColl->setDefaultShortcut(runAction, Qt::CTRL + Qt::Key_Q);
	actColl->addAction(QStringLiteral("cancel"), cancelAction);
	connect(runAction, &QAction::triggered, outputView, &OutputView::cancelProcess);

	auto newFunctionAction = new QAction;
	newFunctionAction->setText(i18n("New Function"));
	newFunctionAction->setIcon(QIcon::fromTheme("list-add"));
	actColl->addAction(QStringLiteral("new-function"), newFunctionAction);
	connect(newFunctionAction, &QAction::triggered, this, &MainWindow::newFunction);

	auto newModuleAction = new QAction;
	newModuleAction->setText(i18n("New Module"));
	newModuleAction->setIcon(QIcon::fromTheme("package-new"));
	actColl->addAction(QStringLiteral("new-module"), newModuleAction);
	connect(newModuleAction, &QAction::triggered, this, &MainWindow::newModule);

	setupGUI(Default, ":/share/kxmlgui5/chiggui/chigguiui.rc");
}

MainWindow::~MainWindow()
{
	mOpenRecentAction->saveEntries(KSharedConfig::openConfig()->group("Recent Files"));
}

void MainWindow::save()
{
	for (int idx = 0; idx < mFunctionTabs->count(); ++idx) {
		auto func = mFunctionTabs->widget(idx);
		if (func != nullptr) {
			auto castedFunc = dynamic_cast<FunctionView*>(func);
			if (castedFunc != nullptr) {
				castedFunc->updatePositions();
			}
		}
	}

	if (mModule != nullptr) {
		chig::Result res = mModule->saveToDisk();
		if (!res) {
			KMessageBox::detailedError(
				this, i18n("Failed to save module!"), QString::fromStdString(res.dump()));
		}
	}
}

void MainWindow::openWorkspaceDialog()
{
	QString workspace = QFileDialog::getOpenFileName(
		this, i18n("Chigraph Workspace"), QDir::homePath(), {}, nullptr, QFileDialog::ShowDirsOnly);

	if (workspace == "") {
		return;
	}

	QUrl url = QUrl::fromLocalFile(workspace);

	mOpenRecentAction->addUrl(url);

	openWorkspace(url);
}

void MainWindow::openWorkspace(QUrl url)
{
	mChigContext = std::make_unique<chig::Context>(url.toLocalFile().toStdString());

	workspaceOpened(*mChigContext);
}

void MainWindow::openModule(const QString& path)
{
	chig::ChigModule* cmod;
	chig::Result res = context().loadModule(path.toStdString(), &cmod);

	if (!res) {
		KMessageBox::detailedError(this, "Failed to load JsonModule from file \"" + path + "\"",
			QString::fromStdString(res.dump()), "Error Loading");

		return;
	}

	mModule = static_cast<chig::JsonModule*>(cmod);
	setWindowTitle(QString::fromStdString(mModule->fullName()));

	// call signal
	moduleOpened(mModule);
}

void MainWindow::newFunctionSelected(chig::GraphFunction* func)
{
	Expects(func);

	QString qualifiedFunctionName =
		QString::fromStdString(func->module().fullName() + ":" + func->name());

	// see if it's already open
	auto funcView = functionView(qualifiedFunctionName);
	if (funcView != nullptr) {
		mFunctionTabs->setCurrentWidget(funcView);
		return;
	}
	// if it's not already open, we'll have to create our own

	auto view = new FunctionView(func, mFunctionTabs);
	int idx = mFunctionTabs->addTab(view, qualifiedFunctionName);
	mOpenFunctions[qualifiedFunctionName] = view;
	mFunctionTabs->setTabText(idx, qualifiedFunctionName);
	mFunctionTabs->setCurrentWidget(view);

	functionOpened(view);
}

void MainWindow::closeTab(int idx)
{
	mOpenFunctions.erase(std::find_if(mOpenFunctions.begin(), mOpenFunctions.end(),
		[&](auto& p) { return p.second == mFunctionTabs->widget(idx); }));
	mFunctionTabs->removeTab(idx);
}

void MainWindow::run()
{
	if (currentModule() == nullptr) {
		return;
	}

	// compile!
	std::unique_ptr<llvm::Module> llmod;
	chig::Result res = context().compileModule(currentModule()->fullName(), &llmod);

	if (!res) {
		KMessageBox::detailedError(
			this, "Failed to compile module", QString::fromStdString(res.dump()));

		return;
	}

	std::string str;
	{
		llvm::raw_string_ostream os(str);

		llvm::WriteBitcodeToFile(llmod.get(), os);
	}

	// run in lli
	auto lliproc = new QProcess(this);
	lliproc->setProgram(QStringLiteral(CHIG_LLI_EXE));
	lliproc->start();
	lliproc->write(str.c_str(), str.length());
	lliproc->closeWriteChannel();

	runStarted(lliproc);
}

void MainWindow::newFunction()
{
	if (currentModule() == nullptr) {
		KMessageBox::error(this, "Load a module before creating a new function");
		return;
	}

	QString newName = QInputDialog::getText(this, i18n("New Function Name"), i18n("Function Name"));

	if (newName == "") {
		return;
	}

	chig::GraphFunction* func;
	currentModule()->createFunction(newName.toStdString(), {}, {}, {""}, {""}, &func);
	func->getOrInsertEntryNode(0, 0, "entry");

	newFunctionCreated(func);
	newFunctionSelected(func);  // open the newly created function
}

void MainWindow::newModule()
{
	// can't do this without a workspace
	if (context().workspacePath().empty()) {
		KMessageBox::error(this, i18n("Cannot create a module without a workspace to place it in"),
			i18n("Failed to create module"));
		return;
	}

	// TODO: validator
	auto fullName = QInputDialog::getText(this, i18n("New Module"), i18n("Full Module Name"));

	auto mod = context().newJsonModule(fullName.toStdString());

	// add lang and c
	mod->addDependency("lang");
	mod->addDependency("c");

	mod->saveToDisk();

	newModuleCreated(mod);
	// then load the module
	openModule(QString::fromStdString(mod->fullName()));
}
