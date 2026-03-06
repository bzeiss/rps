#include <rps/gui/GuiWorkerMain.hpp>
#include "Vst3GuiHost.hpp"

int main(int argc, char* argv[]) {
    auto host = std::make_unique<rps::scanner::Vst3GuiHost>();
    return rps::gui::GuiWorkerMain::run(argc, argv, std::move(host));
}
