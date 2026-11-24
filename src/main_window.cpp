#include "aiws/gui/main_window.hpp"
#include "aiws/document.hpp"
#include "aiws/gui/context_widget.hpp"
#include "aiws/gui/resettable.hpp"
#include "aiws/gui/search_widget.hpp"
#include "aiws/gui/term_inspector_widget.hpp"
#include "aiws/gui/workspace_widget.hpp"

#include <QCloseEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>
#include <exception>
#include <algorithm>
#include <utility>

namespace aiws::gui{

class MainWindow::CorpusState{
public:
    virtual ~CorpusState() = default;
    virtual bool can_build() const {return false;}
    virtual bool can_process() const {return false;}

    virtual std::unique_ptr<CorpusState> build(MainWindow&) const {return {};}
    virtual void started(MainWindow&) const {}
    virtual std::unique_ptr<CorpusState> finished(MainWindow&, BuildController::Result) const{
        return {};
    }
    virtual std::unique_ptr<CorpusState> workspace_changed(MainWindow& window) const;
    virtual std::unique_ptr<CorpusState> reset(MainWindow& window) const;
    virtual void exit(MainWindow& window) const{
        window.shutting_down_ = true;
        window.update_action_states();
        window.ipc_server_.stop();
        window.build_controller_.shutdown();
    }
};

class MainWindow::EmptyState final : public CorpusState{};

class MainWindow::BuildableState : public CorpusState{
public:
    bool can_build() const override {return true;}
    std::unique_ptr<CorpusState> build(MainWindow& window) const override;
};

class MainWindow::DirtyState final : public BuildableState{};

class MainWindow::CurrentState final : public BuildableState{
public:
    bool can_process() const override {return true;}
};

class MainWindow::FailedState final : public BuildableState{};

class MainWindow::BuildingState final : public CorpusState{
public:
    void started(MainWindow& window) const override{
        window.status_label_->setText("Building corpus in background...");
    }

    std::unique_ptr<CorpusState> finished(MainWindow& window, BuildController::Result result) const override{
        if(result.core){
            window.processing_core_ = std::move(*result.core);
            window.workspace_widget_->set_corpus_status(QString("Corpus: %1 chunks").arg(static_cast<qulonglong>(window.processing_core_.chunk_count())));
            window.status_label_->setText("Corpus built.");
            return std::make_unique<CurrentState>();
        }
        window.workspace_widget_->set_corpus_status("Corpus: Failed");
        window.status_label_->setText(QString("Corpus build failed: %1").arg(QString::fromStdString(result.error)));
        return std::make_unique<FailedState>();
    }
};

std::unique_ptr<MainWindow::CorpusState> MainWindow::CorpusState::workspace_changed(
    MainWindow& window) const{
    window.cancel_build();
    window.clear_corpus();
    window.status_label_->setText("Document added. Build the corpus to process it.");
    return std::make_unique<DirtyState>();
}

std::unique_ptr<MainWindow::CorpusState> MainWindow::CorpusState::reset(MainWindow& window) const{
    window.cancel_build();
    window.reset_workspace();
    return std::make_unique<EmptyState>();
}

std::unique_ptr<MainWindow::CorpusState> MainWindow::BuildableState::build(MainWindow& window) const{
    auto next = std::make_unique<BuildingState>();
    window.clear_corpus();
    try{
        window.start_build();
    }catch(const std::exception& error){
        window.workspace_widget_->set_corpus_status("Corpus: Failed");
        window.status_label_->setText(QString("Could not start build: %1").arg(error.what()));
        return std::make_unique<FailedState>();
    }
    window.workspace_widget_->set_corpus_status("Corpus: Building...");
    window.status_label_->setText("Waiting for build worker...");
    return next;
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), state_(std::make_unique<EmptyState>()),
      build_controller_(
          [this](std::size_t id){QMetaObject::invokeMethod(this, [this, id](){ build_started(id); }, Qt::QueuedConnection);},
          [this](BuildController::Result result){
              auto payload = std::make_shared<BuildController::Result>(std::move(result));
              QMetaObject::invokeMethod(this, [this, payload](){finish_build(std::move(*payload));
              }, Qt::QueuedConnection);
          }){
    setWindowTitle("AI Workspace");
    resize(900, 900);
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* layout = new QVBoxLayout(central);
    auto* heading = new QLabel("AI Workspace", central);
    heading->setStyleSheet("background-color: #243b53;"
                           "color: white;"
                           "font-size: 22px;"
                           "font-weight: bold;"
                           "padding: 12px;");
    layout->addWidget(heading);
    workspace_widget_ = new WorkspaceWidget(central);
    layout->addWidget(workspace_widget_, 3);
    search_widget_ = new SearchWidget(central);
    layout->addWidget(search_widget_, 4);
    auto* bottom_layout = new QHBoxLayout();
    term_inspector_widget_ = new TermInspectorWidget(central);
    context_widget_ = new ContextWidget(central);
    bottom_layout->addWidget(term_inspector_widget_, 1);
    bottom_layout->addWidget(context_widget_, 1);
    layout->addLayout(bottom_layout, 3);
    auto* action_layout = new QHBoxLayout();
    status_label_ = new QLabel("Ready.", central);
    status_label_->setWordWrap(true);
    status_label_->setTextFormat(Qt::PlainText);
    reset_button_ = new QPushButton("Reset", central);
    exit_button_ = new QPushButton("Exit", central);
    reset_button_->setStyleSheet("background-color: #b8d8ed;"
                                 "color: #102030;"
                                 "padding: 6px 16px;");
    exit_button_->setStyleSheet("background-color: #8b2635;"
                                "color: white;"
                                "padding: 6px 16px;");
    action_layout->addWidget(status_label_, 1);
    action_layout->addWidget(reset_button_);
    action_layout->addWidget(exit_button_);
    layout->addLayout(action_layout);
    connect(workspace_widget_, &WorkspaceWidget::add_document_requested, this, &MainWindow::add_document);
    connect(workspace_widget_, &WorkspaceWidget::build_corpus_requested, this, &MainWindow::build_corpus);
    connect(workspace_widget_, &WorkspaceWidget::document_selected, this, &MainWindow::document_selected);
    connect(search_widget_, &SearchWidget::search_requested, this, &MainWindow::search_requested);
    connect(search_widget_, &SearchWidget::query_changed, this, &MainWindow::query_changed);
    connect(search_widget_, &SearchWidget::source_document_requested, this,
            &MainWindow::source_document_requested);
    connect(search_widget_, &SearchWidget::selected_chunk_changed, term_inspector_widget_,
            &TermInspectorWidget::set_selected_chunk);
    connect(term_inspector_widget_, &TermInspectorWidget::term_inspection_requested, this,
            &MainWindow::term_inspection_requested);
    connect(context_widget_, &ContextWidget::build_context_requested, this,
            &MainWindow::build_context_requested);
    connect(reset_button_, &QPushButton::clicked, this, &MainWindow::reset_application);
    connect(exit_button_, &QPushButton::clicked, this, &MainWindow::exit_application);

    update_action_states();
}

MainWindow::~MainWindow(){
    shutting_down_ = true;
    // Stop the server before destroying the window.
    ipc_server_.stop();
}

void MainWindow::set_exit_callback(ExitCallback callback) noexcept{
    exit_callback_ = callback;
}

void MainWindow::add_document(){
    const QString path =
        QFileDialog::getOpenFileName(this, "Add Document", QString(), "Text files (*.txt);;All files (*)");
    if(!path.isEmpty() && !add_document_file(path)){
        QMessageBox::warning(this, "Add Document Error", "Could not read the selected file.");
    }
}

bool MainWindow::add_document_file(const QString& path){
    if(shutting_down_ || !QFileInfo(path).isFile()){
        return false;
    }
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)){
        return false;
    }
    const QByteArray contents = file.readAll();
    if(file.error() != QFileDevice::NoError){
        return false;
    }

    const QFileInfo info(path);
    try{
        const QString title = info.fileName();
        workspace_.add_document(
            Document(info.absoluteFilePath().toStdString(), title.toStdString(), contents.toStdString()));
        workspace_widget_->add_document(title);
        enter_state(state_->workspace_changed(*this));
        return true;
    }catch(const std::exception&){
        return false;
    }
}

bool MainWindow::start_ipc_server(const QString& socket_path){
    std::string error;
    // Add the document on the GUI thread.
    const bool started = ipc_server_.start(
        socket_path.toStdString(),
        [this](const std::string& path){
            QMetaObject::invokeMethod(
                this,
                [this, path](){
                    if(!shutting_down_){
                        add_document_file(QString::fromStdString(path));
                    }
                },
                Qt::QueuedConnection);
        },
        error);
    if(!started){
        status_label_->setText(QString("Remote Add unavailable: %1").arg(QString::fromStdString(error)));
    }
    return started;
}

void MainWindow::set_build_delay_ms(int ms_per_document){
    build_delay_ms_ = std::max(0, ms_per_document);
}

void MainWindow::build_corpus(){
    if(!shutting_down_){
        enter_state(state_->build(*this));
    }
}

void MainWindow::start_build(){
    const auto id = ++next_build_id_;
    build_controller_.start(workspace_, id, build_delay_ms_);
    wanted_build_ = id;
}

void MainWindow::build_started(std::size_t id){
    if(!shutting_down_ && wanted_build_ == id){
        state_->started(*this);
    }
}

void MainWindow::finish_build(BuildController::Result result){
    if(shutting_down_){
        return;
    }
    if(!result.cancelled && wanted_build_ == result.id){
        wanted_build_.reset();
        enter_state(state_->finished(*this, std::move(result)));
    }
    emit build_finished();
}

void MainWindow::enter_state(std::unique_ptr<CorpusState> next){
    if(next){
        state_ = std::move(next);
        update_action_states();
    }
}

void MainWindow::document_selected(int row){
    const auto& documents = workspace_.documents();
    if(row < 0 || static_cast<std::size_t>(row) >= documents.size()){
        workspace_widget_->clear_selected_document();
        return;
    }
    const Document& document = documents[static_cast<std::size_t>(row)];
    workspace_widget_->show_selected_document(QString::fromStdString(document.title()),
                                              QString::fromStdString(document.text()));
}

void MainWindow::search_requested(const QString& query, int k){
    if(shutting_down_ || !state_->can_process() || query.trimmed().isEmpty()){
        return;
    }
    try{
        auto results = processing_core_.search(query.toStdString(), k);
        clear_processing_outputs();
        search_widget_->set_results(results);
        status_label_->setText(
            QString("Search complete: %1 results.").arg(static_cast<qulonglong>(results.size())));
    }catch(const std::exception& error){
        QMessageBox::warning(this, "Search Error", QString::fromUtf8(error.what()));
    }
}

void MainWindow::query_changed(const QString& query){
    (void)query;
    // Only update the buttons when the query changes.
    update_action_states();
}

void MainWindow::build_context_requested(int token_budget){
    if(shutting_down_ || !state_->can_process() || !search_widget_->has_query() || token_budget <= 0){
        return;
    }
    try{
        const auto items = processing_core_.build_context(search_widget_->query().toStdString(),
                                                          search_widget_->result_count(),
                                                          static_cast<std::size_t>(token_budget));
        context_widget_->set_context(items);
        status_label_->setText("Context built.");
    }catch(const std::exception& error){
        QMessageBox::warning(this, "Context Error", QString::fromUtf8(error.what()));
    }
}

void MainWindow::source_document_requested(const QString& document_id){
    const auto& documents = workspace_.documents();
    for(std::size_t row = 0; row < documents.size(); row++){
        if(documents[row].id() == document_id.toStdString()){
            workspace_widget_->select_document(static_cast<int>(row));
            document_selected(static_cast<int>(row));
            return;
        }
    }
}

void MainWindow::term_inspection_requested(const QString& term, const QString& chunk_id){
    if(shutting_down_ || !state_->can_process() || term.trimmed().isEmpty()){
        return;
    }
    try{
        const std::size_t df = processing_core_.document_frequency(term.toStdString());
        const bool has_chunk = !chunk_id.isEmpty();
        std::size_t tf = 0;
        if(has_chunk){
            tf = processing_core_.term_frequency(term.toStdString(), chunk_id.toStdString());
        }
        term_inspector_widget_->set_statistics(df, has_chunk, tf);
        status_label_->setText("Term inspected.");
    }catch(const std::exception& error){
        term_inspector_widget_->clear_statistics();
        QMessageBox::warning(this, "Inspection Error", QString::fromUtf8(error.what()));
    }
}

void MainWindow::reset_application(){
    if(!shutting_down_){
        enter_state(state_->reset(*this));
    }
}

void MainWindow::reset_workspace(){
    workspace_.clear();
    processing_core_ = ProcessingCore();
    Resettable* components[] = {workspace_widget_, search_widget_, term_inspector_widget_, context_widget_};
    for(Resettable* component : components){
        component->reset();
    }
    status_label_->setText("Application reset.");
}

void MainWindow::exit_application(){
    if(exit_callback_ != nullptr){
        exit_callback_(this);
    }
}

void MainWindow::closeEvent(QCloseEvent* event){
    if(!shutting_down_){
        state_->exit(*this);
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::update_action_states(){
    workspace_widget_->set_build_corpus_enabled(!shutting_down_ && state_->can_build());
    const bool corpus_available = !shutting_down_ && state_->can_process();
    search_widget_->set_search_enabled(corpus_available && search_widget_->has_query());
    context_widget_->set_build_context_enabled(corpus_available && search_widget_->has_query());
    term_inspector_widget_->set_inspection_enabled(corpus_available);
}

void MainWindow::cancel_build(){
    wanted_build_.reset();
    build_controller_.cancel();
}

void MainWindow::clear_corpus(){
    workspace_widget_->set_corpus_status("Corpus: Not built");
    clear_processing_outputs();
}

void MainWindow::clear_processing_outputs(){
    search_widget_->clear_results();
    term_inspector_widget_->set_selected_chunk(QString());
    term_inspector_widget_->clear_statistics();
    context_widget_->clear_context();
}

} // namespace aiws::gui
