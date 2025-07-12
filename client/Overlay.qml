// resources/Overlay.qml
import QtQuick 2.15
import QtQuick.Controls 2.15

Item {
    id: root
    anchors.fill: parent        // always cover the whole wrapper

    Rectangle {                 // translucent dark bar
        id: bar
        color: "#66000000"
        height: 28
        width: parent.width
        anchors.bottom: parent.bottom
        Row {
            anchors.centerIn: parent
            spacing: 8
            Button { text: "Pause"  onClicked: pause()  }
            Button { text: "Stop"   onClicked: stop()   }
        }
    }

    /* Expose signals to C++ if you want: */
    signal pause()
    signal stop()
}
