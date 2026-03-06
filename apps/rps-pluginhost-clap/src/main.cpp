#include <rps/gui/GuiWorkerMain.hpp>
#include "ClapGuiHost.hpp"

int main(int argc, char* argv[]) {
    auto host = std::make_unique<rps::scanner::ClapGuiHost>();
    return rps::gui::GuiWorkerMain::run(argc, argv, std::move(host));
}
