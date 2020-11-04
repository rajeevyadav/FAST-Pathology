#include <FAST/Tools/CommandLineParser.hpp>
#include "MainWindow.hpp"
#include <QApplication>
#include <QtCore>
#include <QtGui>

using namespace fast;

int main(int argc, char** argv) {
    CommandLineParser parser("FastPathology", "Add description Andre");
    parser.addOption("verbose", "Print info messages");
    parser.parse(argc, argv);

    if(parser.getOption(("verbose")))
        Reporter::setGlobalReportMethod(Reporter::COUT);

    // Setup window
    auto window = MainWindow::New();
	//Q_INIT_RESOURCE(qtres);
    window->start();
}

