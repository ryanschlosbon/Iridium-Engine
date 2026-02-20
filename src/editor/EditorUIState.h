#pragma once

struct EditorUIState {
    // We can add as many booleans here as we want in the future!
    bool showProjectSettings = false;
    bool showPhysicsDebugger = false;

    // We will use this one to test the architecture right now
    bool showDemoWindow = false;
};