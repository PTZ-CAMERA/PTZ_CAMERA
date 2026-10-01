#include <QApplication>
#include <QDebug>
#include <QUrl>
#include <QWebEngineView>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    const QUrl streamUrl(argc > 1
                             ? QString::fromLocal8Bit(argv[1])
                             : QStringLiteral("http://192.168.0.92:8889/cam/"));

    QWebEngineView view;
    view.setWindowTitle(QStringLiteral("PTZ camera connection test"));
    view.resize(960, 600);
    QObject::connect(&view, &QWebEngineView::loadFinished, &view,
                     [streamUrl](bool ok) {
                         if (!ok) qWarning() << "Could not load" << streamUrl;
                     });
    view.load(streamUrl);
    view.show();

    return app.exec();
}
