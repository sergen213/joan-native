#include "ui_cases_frontend.hpp"

int main() { return uishot::run_cases(joa::engine::RendererKind::Vulkan, kFrontEndCases); }
