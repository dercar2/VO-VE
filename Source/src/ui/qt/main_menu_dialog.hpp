#pragma once

class QWidget;

namespace vove::ui {

// Owner-scoped: only descendant QDialogs opting in with mainMenuDialog are decorated.
// Install before constructing dialogs; native pickers and file-operation prompts stay unchanged.
void install_main_menu_dialog_frames(QWidget *owner);

} // namespace vove::ui
