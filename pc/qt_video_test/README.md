# PC Qt video connection test

This standalone Qt 6 Widgets project loads the Pi's MediaMTX WebRTC page.
Open `CMakeLists.txt` in Qt Creator on the PC, select a Qt 6 kit with
WebEngineWidgets installed, build, and run. The URL can be passed as the first
command-line argument; otherwise it uses `http://192.168.0.92:8889/cam/`.

First open that URL in a normal PC browser. The PC must reach the Pi on TCP
8889 and UDP 8189. Run `hostname -I` on the Pi if its LAN address changes.

To add video to an existing Qt Widgets screen, place a `QWebEngineView` in its
layout and call:

```cpp
view->load(QUrl("http://192.168.0.92:8889/cam/"));
```

For a Qt Quick/QML screen:

```qml
import QtWebEngine

WebEngineView {
    anchors.fill: parent
    url: "http://192.168.0.92:8889/cam/"
}
```

For HTML shown inside Qt WebEngine:

```html
<iframe src="http://192.168.0.92:8889/cam/"
        allow="autoplay; fullscreen"></iframe>
```

This is only a video connection test. It does not include pan/tilt controls.
