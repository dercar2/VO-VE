#pragma once

#include "components_panel.hpp"

#include <functional>
#include <vector>

class QWidget;

namespace vove::ui {

using ComponentSpecsProvider = std::function<std::vector<ComponentSpec>(bool refresh)>;
using RefreshComponents = std::function<void(std::function<void()>)>;

void show_project_contact_dialog(QWidget *owner, bool support);
void show_project_components_dialog(QWidget *owner, ComponentSpecsProvider specs,
                                    RefreshComponents refresh);

} // namespace vove::ui
