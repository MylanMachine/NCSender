#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("NCSender"));
    QApplication::setOrganizationName(QStringLiteral("NCSender"));
    QApplication::setApplicationVersion(QStringLiteral(NCSENDER_VERSION));
    Theme::apply(app);

    MainWindow window;
    window.show();
    return app.exec();
}
