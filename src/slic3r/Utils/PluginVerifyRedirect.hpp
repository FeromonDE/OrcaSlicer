#pragma once

namespace Slic3r {

// Installs a narrow Windows API redirect used only by Bambu's network plug-in.
// Kept separate from Orca's normal networking path so non-Bambu launches remain unchanged.
// Calls originating from bambu_networking/BambuSource that ask Windows to verify
// Orca's unsigned studio module are redirected to the currently installed,
// genuinely Bambu-signed BambuStudio.dll / bambu-studio.exe.
void install_plugin_verify_redirect();

// CI trigger after Actions permissions enabled.
} // namespace Slic3r
