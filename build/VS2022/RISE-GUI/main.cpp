//////////////////////////////////////////////////////////////////////
//
//  main.cpp - Entry point for the RISE Qt GUI application
//
//////////////////////////////////////////////////////////////////////

#include <QApplication>
#include <QStyleFactory>
#include "MainWindow.h"

int main(int argc, char* argv[])
{
    // Request the Windows 11 Fluent Design style (rounded corners,
    // Mica backdrop, system accent colors).  Falls back gracefully
    // to the default Windows style on older OS versions.
    if (QStyleFactory::keys().contains("windows11", Qt::CaseInsensitive)) {
        QApplication::setStyle("windows11");
    }

    QApplication app(argc, argv);
    app.setApplicationName("RISE");
    app.setOrganizationName("RISE");

    MainWindow window;
    window.resize(1024, 768);
    window.show();

    return app.exec();
}
