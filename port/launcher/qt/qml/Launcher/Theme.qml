pragma Singleton
import QtQuick
import Launcher

// Every color, size and font of the launcher lives here (single source for the visual identity).
QtObject {
    // surfaces
    readonly property color bg: "#0f1316"
    readonly property color rail: "#0b0f11"
    readonly property color railLine: "#1c2428"
    readonly property color navHover: "#1a1d23"
    readonly property color navOn: "#1d2027"
    readonly property color navOnLine: "#2c313a"
    readonly property color card: "#141a1d"
    readonly property color cardLine: "#25303a"
    readonly property color chip: "#171a20"
    readonly property color chipLine: "#2a2f38"
    readonly property color ghost: "#1a1d23"
    readonly property color ghostHover: "#22262e"
    readonly property color ghostLine: "#303641"
    readonly property color ghostLineHover: "#444b59"
    // text
    readonly property color text: "#ece6da"
    readonly property color textSoft: "#c9c5bd"
    readonly property color textChip: "#d8d5ce"
    readonly property color textMuted: "#a3a9b3"
    readonly property color textDim: "#8e949f"
    readonly property color textFaint: "#7d838e"
    // states
    readonly property color ok: "#35d07f"
    readonly property color warn: "#f0c35a"   // late-afternoon sun
    readonly property color bad: "#ff4d4d"
    readonly property color off: "#5a606b"
    // one accent per section
    readonly property color smoke: "#e8a24a"  // Play, Fixes: the Nest's sunset amber
    readonly property color video: "#6fb3d4"  // Graphics: valley sky
    readonly property color neon: "#d9876a"   // Controls: Trico's feathers
    readonly property color cyan: "#7fd3c4"   // Support: the barrels' glow
    readonly property var accents: ({ play: "#e8a24a", graphics: "#6fb3d4", controls: "#d9876a",
                                      fixes: "#e8a24a", saves: "#7cc47f", guides: "#d9876a", support: "#7fd3c4",
                                      updates: "#f0c35a", advanced: "#a99be0", mods: "#9fc9a4" })
    // type
    property string fontDisplay: Backend.fontDisplay
    property string fontText: Backend.fontText
    readonly property int railWidth: 236
    readonly property int heroHeight: 320
    // line icons, 24x24 viewBox (circles written as arcs)
    readonly property var icons: ({
        play: "M7 4.5v15l12-7.5z",
        graphics: "M4 7h10M18 7h2M4 17h2M10 17h10 M18 7a2 2 0 1 0-4 0a2 2 0 1 0 4 0 M10 17a2 2 0 1 0-4 0a2 2 0 1 0 4 0",
        controls: "M7 8h10a4 4 0 0 1 4 4l-1 5a2 2 0 0 1-3.4 1L15 16H9l-1.6 2A2 2 0 0 1 4 17l-1-5a4 4 0 0 1 4-4z",
        fixes: "M12 3l8 3v6c0 4.5-3.2 7.8-8 9-4.8-1.2-8-4.5-8-9V6z M8.5 12l2.5 2.5 4.5-5",
        guides: "M7 8h10a4 4 0 0 1 4 4l-1 5a2 2 0 0 1-3.4 1L15 16H9l-1.6 2A2 2 0 0 1 4 17l-1-5a4 4 0 0 1 4-4z M8 10.5v3M6.5 12h3 M15.5 11.5h.01M17.5 13.5h.01",
        saves: "M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z M12 10.5v5M9.5 13.5 12 16l2.5-2.5",
        support: "M21 12a9 9 0 1 0-18 0a9 9 0 1 0 18 0 M15.5 12a3.5 3.5 0 1 0-7 0a3.5 3.5 0 1 0 7 0 M5.6 5.6l3.9 3.9M14.5 14.5l3.9 3.9M18.4 5.6l-3.9 3.9M9.5 14.5l-3.9 3.9",
        updates: "M12 4v11M7.5 11 12 15.5 16.5 11M5 20h14",
        advanced: "M4 6h16M4 12h16M4 18h16 M9 6a2 2 0 1 0-4 0a2 2 0 1 0 4 0 M19 12a2 2 0 1 0-4 0a2 2 0 1 0 4 0 M12 18a2 2 0 1 0-4 0a2 2 0 1 0 4 0",
        mods: "M6 9h12a3 3 0 0 1 0 6H6a3 3 0 0 1 0-6z M17.2 12a2.2 2.2 0 1 0-4.4 0a2.2 2.2 0 1 0 4.4 0"
    })

    // Translation: reading Backend.lang inside makes every binding that calls it re-evaluate
    // when the player switches language.
    function tr(key) { var l = Backend.lang; return Backend.t(key) }
}
