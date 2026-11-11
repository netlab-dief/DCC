#pragma once
#include "dcc/dcc_access.h"
#include <string>

std::unique_ptr<DccRadio> make_linux_dcc_radio(std::string survey, std::string monitor, bool fake_cbr);
