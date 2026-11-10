#pragma once
#include <QString>

// The browser source (Chromium, for web pages and StreamElements/Streamlabs overlays) comes with the installer but is
// loaded only once someone uses it, so it costs nothing for everyone else.
namespace Addons {
QString coreDir();            // where modules live: <app>\core
bool browserInstalled();      // the files are there
bool browserWanted();         // switched on, or the scene collection already has a browser source
void setBrowserEnabled(bool on);
} // namespace Addons
