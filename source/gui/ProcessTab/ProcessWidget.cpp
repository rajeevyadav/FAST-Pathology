#include "ProcessWidget.h"
#include <FAST/Importers/WholeSlideImageImporter.hpp>
#include <FAST/Visualization/ImagePyramidRenderer/ImagePyramidRenderer.hpp>
#include <FAST/Data/ImagePyramid.hpp>
#include <FAST/Visualization/SegmentationRenderer/SegmentationRenderer.hpp>
#include <QGLContext>
#include <QProgressDialog>
#include <QThread>
#include <FAST/Visualization/View.hpp>
#include <FAST/Visualization/ComputationThread.hpp>
#include <FAST/Algorithms/ImagePatch/PatchGenerator.hpp>
#include "source/logic/WholeSlideImage.h"
#include "source/gui/MainWindow.hpp"

namespace fast {
    ProcessWidget::ProcessWidget(MainWindow* mainWindow, QWidget* parent): QWidget(parent){
        m_mainWindow = mainWindow;
        m_computationThread = mainWindow->getComputationThread();
        this->_cwd = join(QDir::homePath().toStdString(), "fastpathology");
        m_view = mainWindow->getView(0);
        this->setupInterface();
        this->setupConnections();

        // Notify GUI when pipeline has finished
        QObject::connect(m_computationThread.get(), &ComputationThread::pipelineFinished, this, &ProcessWidget::done);

        // Stop whe critical error occurs in computation thread
        QObject::connect(m_computationThread.get(), &ComputationThread::criticalError, this, &ProcessWidget::stop);

        // Connection to show message in GUI in main thread
        QObject::connect(this, &ProcessWidget::messageSignal, this, &ProcessWidget::showMessage, Qt::QueuedConnection);
    }

    ProcessWidget::~ProcessWidget(){

    }

    void ProcessWidget::setupInterface()
    {
        this->_main_layout = new QVBoxLayout(this);
        this->_main_layout->setSpacing(6);
        this->_main_layout->setAlignment(Qt::AlignTop);

        auto label = new QLabel();
        label->setText("Select processing pipeline:");
        _main_layout->addWidget(label);

        _stacked_layout = new QStackedLayout;

        _stacked_widget= new QWidget(this);
        _stacked_widget->setLayout(_stacked_layout);

        _page_combobox = new QComboBox(this);

        _main_layout->addWidget(_page_combobox);
        _main_layout->addWidget(_stacked_widget);

        _main_layout->addStretch();

        auto addPipelinesButton = new QPushButton();
        addPipelinesButton->setText("Add pipelines from disk");
        _main_layout->addWidget(addPipelinesButton);
        connect(addPipelinesButton, &QPushButton::clicked, this, &ProcessWidget::addPipelinesFromDisk);

        auto addModelsButton = new QPushButton();
        addModelsButton->setText("Add models from disk");
        _main_layout->addWidget(addModelsButton);
        connect(addModelsButton, &QPushButton::clicked, this, &ProcessWidget::addModelsFromDisk);

        this->refreshPipelines();
    }

    void ProcessWidget::resetInterface()
    {
        _page_combobox->setCurrentIndex(0);
        _stacked_layout->setCurrentIndex(0);
    }

    void ProcessWidget::setupConnections()
    {
        QObject::connect(_page_combobox, SIGNAL(activated(int)), _stacked_layout, SLOT(setCurrentIndex(int)));
    }

    void ProcessWidget::refreshPipelines(QString currentFilename) {
        _page_combobox->clear();
        clearLayout(_stacked_layout);
        resetInterface();
        int index = 0;
        int counter = 0;
        // Load pipelines and create one button for each.
        std::string pipelineFolder = this->_cwd + "/pipelines/";
        for(auto& filename : getDirectoryList(pipelineFolder)) {
            try {
                auto pipeline = Pipeline(join(pipelineFolder, filename));

                if(filename == currentFilename.toStdString()) {
                    index = counter;
                }

                auto page = new QWidget();
                auto layout = new QVBoxLayout();
                layout->setAlignment(Qt::AlignTop);
                page->setLayout(layout);
                _stacked_layout->addWidget(page);
                _page_combobox->addItem(QString::fromStdString(pipeline.getName()));

                auto description = new QLabel();
                description->setText(QString::fromStdString(pipeline.getDescription()));
                description->setWordWrap(true);
                layout->addWidget(description);

                auto button = new QPushButton;
                button->setText("Run pipeline for this image");
                button->setStyleSheet("background-color: #ADD8E6;");
                layout->addWidget(button);
                QObject::connect(button, &QPushButton::clicked, [=]() {
                    runInThread(join(pipelineFolder, filename), pipeline.getName(), false);
                });

                auto batchButton = new QPushButton;
                batchButton->setText("Run pipeline for all images");
                layout->addWidget(batchButton);
                QObject::connect(batchButton, &QPushButton::clicked, [=]() {
                    runInThread(join(pipelineFolder, filename), pipeline.getName(), true);
                });

                layout->addSpacing(20);

                auto editButton = new QPushButton;
                editButton->setText("Edit pipeline");
                layout->addWidget(editButton);
                connect(editButton, &QPushButton::clicked, [=]() {
                    auto editor = new PipelineScriptEditorWidget(QString::fromStdString(pipeline.getFilename()), this);
                    connect(editor, &PipelineScriptEditorWidget::pipelineSaved, this, &ProcessWidget::refreshPipelines);
                });
                ++counter;
            } catch(std::exception &e) {
                Reporter::warning() << "Unable to read pipeline file " << filename << ", ignoring.." << Reporter::end();
                continue;
            }
        }
        _page_combobox->setCurrentIndex(index);
        _stacked_layout->setCurrentIndex(index);
    }

    void ProcessWidget::runInThread(std::string pipelineFilename, std::string pipelineName, bool runForAll) {
        stopProcessing(); // Have to stop any renderers etc first.

        // Create a GL context for the thread which is sharing with the context of the view
        // We have to this because we need an OpenGL context when parsing the pipeline and thereby creating renderers
        auto view = m_view;
        auto context = new QGLContext(View::getGLFormat(), view);
        context->create(view->context());

        if (!context->isValid())
            throw Exception("The custom Qt GL context is invalid!");

        if (!context->isSharing())
            throw Exception("The custom Qt GL context is not sharing!");

        auto thread = new QThread();
        context->makeCurrent();
        context->doneCurrent();
        context->moveToThread(thread);
        QObject::connect(thread, &QThread::started, [=](){
            context->makeCurrent();
            if (runForAll) {
                batchProcessPipeline(pipelineFilename);
            } else {
                processPipeline(pipelineFilename, nullptr);
            }
            context->doneCurrent(); // Must call done here for some reason..
            thread->quit();
        });
        int size = m_mainWindow->getCurrentProject()->getWSICountInProject();
        // TODO lock tabs etc while running.
        m_progressDialog = new QProgressDialog(("Running pipeline " + pipelineName + " ..").c_str(), "Cancel", 0, runForAll ? size*100 : 100, this);
        m_progressDialog->setWindowTitle("Running..");
        m_progressDialog->setAutoClose(true);
        m_progressDialog->show();
        auto timer = new QTimer();
        timer->setInterval(200);
        timer->setSingleShot(false);
        QObject::connect(timer, &QTimer::timeout, this, &ProcessWidget::updateProgress);
        QObject::connect(m_progressDialog, &QProgressDialog::finished, timer, &QTimer::stop);
        QObject::connect(m_progressDialog, &QProgressDialog::canceled, timer, &QTimer::stop);
        QObject::connect(m_progressDialog, &QProgressDialog::canceled, [this, thread]() {
            std::cout << "canceled.." << std::endl;
            thread->wait();
            std::cout << "done waiting" << std::endl;
            stop();
        });
        thread->start();
        timer->start();
        // TODO should delete QThread safely somehow..
    }

    void ProcessWidget::stopProcessing() {
        m_procesessing = false;
        std::cout << "stopping pipeline" << std::endl;
        m_view->stopPipeline();
        std::cout << "done" << std::endl;
        std::cout << "removing renderers.." << std::endl;
        m_view->removeAllRenderers();
        std::cout << "done" << std::endl;
    }

    void ProcessWidget::stop() {
        if(m_progressDialog)
            m_progressDialog->setValue(m_progressDialog->maximum()); // Close progress dialog
        m_batchProcesessing = false;
        stopProcessing();
        selectWSI(m_mainWindow->getCurrentWSI()->get_image_pyramid());
    }

    void ProcessWidget::showMessage(QString msg) {
        // This must happen in main thread
        QMessageBox msgBox;
        msgBox.setText(msg);
        msgBox.exec();
    }

    void ProcessWidget::updateProgress() {
        if(m_procesessing && m_runningPipeline && m_runningPipeline->isParsed()) {
            std::vector<std::shared_ptr<PatchGenerator>> currentPatchGenerators;
            for(auto PO : m_runningPipeline->getProcessObjects()) {
                if(auto generator = std::dynamic_pointer_cast<PatchGenerator>(PO.second)) {
                    currentPatchGenerators.push_back(generator);
                }
            }
            if(currentPatchGenerators.empty())
                return;
            float totalProgress = 0;
            for(auto generator : currentPatchGenerators) {
                totalProgress += std::floor(generator->getProgress()*100);
            }
            if(m_progressDialog != nullptr) {
                if(m_batchProcesessing) {
                    m_progressDialog->setValue(std::floor(m_currentWSI*100 + totalProgress/currentPatchGenerators.size()));
                } else {
                    m_progressDialog->setValue(std::floor(totalProgress/currentPatchGenerators.size()));
                }
            }
        }
    }

    void ProcessWidget::done() {
        if(m_procesessing)
            saveResults();
        if(m_batchProcesessing) {
            if(m_currentWSI == m_mainWindow->getCurrentProject()->getWSICountInProject()-1) {
                // All processed. Stop.
                m_progressDialog->setValue(m_progressDialog->maximum());
                m_progressDialog->close();
                m_batchProcesessing = false;
                m_procesessing = false;
                emit messageSignal("Batch processing is done!");
                emit pipelineFinished(m_mainWindow->getCurrentProject()->getAllWsiUids()[0]);
            } else {
                // Run next
                m_currentWSI += 1;
                m_progressDialog->setValue(m_currentWSI*100);
                std::cout << "Processing WSI " << m_currentWSI << std::endl;
                processPipeline(m_runningPipeline->getFilename(), m_mainWindow->getCurrentProject()->getImage(m_currentWSI)->get_image_pyramid());
            }
        } else if(m_procesessing) {
            m_progressDialog->setValue(m_progressDialog->maximum());
            m_progressDialog->close();
            emit messageSignal("Processing is done!");
            emit pipelineFinished(m_mainWindow->getCurrentProject()->getAllWsiUids()[m_currentWSI]);
            m_procesessing = false;
        }
    }

    void ProcessWidget::processPipeline(std::string pipelinePath, std::shared_ptr<ImagePyramid> WSI) {
        std::cout << "Processing pipeline: " << pipelinePath << std::endl;
        stopProcessing();
        m_procesessing = true;
        auto view = m_view;

        // Load pipeline and give it a WSI
        std::cout << "Loading pipeline in thread: " << std::this_thread::get_id() << std::endl;
        m_runningPipeline = std::make_shared<Pipeline>(pipelinePath);
        std::cout << "OK" << std::endl;
        try {
            std::cout << "parsing" << std::endl;
            if(!WSI) {
                auto uids = m_mainWindow->getCurrentProject()->getAllWsiUids();
                auto currentUID = m_mainWindow->getCurrentWSIUID();
                if(currentUID.empty())
                    return;
                for(int i = 0; i < uids.size(); ++i) {
                    if(uids[i] == currentUID) {
                        m_currentWSI = i;
                    }
                }
                WSI = m_mainWindow->getCurrentProject()->getImage(currentUID)->get_image_pyramid();
            }
            m_runningPipeline->parse({{"WSI", WSI}});
            std::cout << "OK" << std::endl;
        } catch(Exception &e) {
            m_procesessing = false;
            m_batchProcesessing = false;
            m_runningPipeline.reset();
            // Syntax error in pipeline file. Raise error and return to avoid crash.
            std::string msg = "Error parsing pipeline! " + std::string(e.what());
            emit messageSignal(msg.c_str());
            return;
        }
        std::cout << "Done" << std::endl;

        for(auto renderer : m_runningPipeline->getRenderers()) {
            view->addRenderer(renderer);
        }
        m_computationThread->reset();
    }

    void ProcessWidget::batchProcessPipeline(std::string pipelineFilename) {
        m_currentWSI = 0;
        m_batchProcesessing = true;
        auto uid = m_mainWindow->getCurrentProject()->getAllWsiUids()[m_currentWSI];
        processPipeline(pipelineFilename, m_mainWindow->getCurrentProject()->getImage(uid)->get_image_pyramid());
    }

    void ProcessWidget::selectWSI(std::shared_ptr<ImagePyramid> WSI) {
        stopProcessing();
        auto view = m_view;
        view->removeAllRenderers();
        auto renderer = ImagePyramidRenderer::create()
                ->connect(WSI);
        view->addRenderer(renderer);
    }

    void ProcessWidget::saveResults() {
        auto pipelineData = m_runningPipeline->getAllPipelineOutputData();
        m_mainWindow->getCurrentProject()->saveResults(m_mainWindow->getCurrentProject()->getAllWsiUids()[m_currentWSI], m_runningPipeline, pipelineData);
    }

    void ProcessWidget::editorPipelinesReceived()
    {
        auto editor = new PipelineScriptEditorWidget(this);
    }


    void ProcessWidget::addModelsFromDisk() {
        // TODO enable selection of folders (TensorFlow SavedModel format)
        QStringList ls = QFileDialog::getOpenFileNames(
                nullptr,
                tr("Select Model"), nullptr,
                tr("Model Files (*.pb *.xml *.bin *.uff *.onnx"),
                nullptr, QFileDialog::DontUseNativeDialog
        ); // DontUseNativeDialog - this was necessary because I got wrong paths -> /run/user/1000/.../filename instead of actual path

        auto progDialog = new QProgressDialog(nullptr);
        progDialog->setRange(0, ls.count() - 1);
        progDialog->setAutoClose(true);
        progDialog->setLabelText("Adding models...");

        int counter = 0;
        // now iterate over all selected files and add selected files and corresponding ones to Models/
        for (QString& filename : ls) {
            std::string filepath = filename.toStdString();
            QString newPath = QString::fromStdString(join(m_mainWindow->getRootFolder(), "models", getFileName(filepath)));
            if(QDir().exists(newPath)) {
                QMessageBox::warning(nullptr, "File exists", "File " + newPath + " exists and will not be copied.");
            } else {
                std::cout << "copying " << filepath << " to " << newPath.toStdString() << std::endl;
                QFile::copy(filename, newPath);
            }
            counter++;
            progDialog->setValue(counter);
        }
        progDialog->close();
    }

    void ProcessWidget::addPipelinesFromDisk() {

        QStringList ls = QFileDialog::getOpenFileNames(
                nullptr,
                tr("Select pipeline to add"), nullptr,
                tr("Pipeline Files (*.fpl)"),
                nullptr, QFileDialog::DontUseNativeDialog
        );

        // now iterate over all selected files and add selected files and corresponding ones to Pipelines/
        for (QString& fileName : ls) {

            if (fileName == "")
                continue;

            std::string someFile = getFileName(fileName.toStdString());
            std::string oldLocation = split(fileName.toStdString(), someFile)[0];
            std::string newLocation = join(m_mainWindow->getRootFolder(), "pipelines");
            std::string newPath = join(newLocation,  someFile);
            if (fileExists(newPath)) {
                QMessageBox::warning(nullptr, "Error", "A pipeline with the filename " + QString::fromStdString(someFile) + " already exists.");
            } else {
                QFile::copy(fileName, QString::fromStdString(newPath));
            }
        }
        refreshPipelines();
    }
}
