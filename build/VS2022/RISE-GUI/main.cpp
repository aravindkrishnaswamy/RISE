//////////////////////////////////////////////////////////////////////
//
//  main.cpp - Entry point for the RISE Qt GUI application
//
//////////////////////////////////////////////////////////////////////

#include <QApplication>
#include "MainWindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("RISE");
    app.setOrganizationName("RISE");

    MainWindow window;
    window.resize(1024, 768);
    window.show();

    return app.exec();
}
