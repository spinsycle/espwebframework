#include "CommandJson.h"

namespace espwf {

CmdErr parseCommandJson(JsonVariantConst json, const char*& id, Command& out) {
  id = nullptr;
  out = Command();
  if (!json["id"].is<const char*>()) return CmdErr::BadId;
  id = json["id"].as<const char*>();
  if (!json["action"].is<const char*>() || !parseAction(json["action"].as<const char*>(), out.action)) {
    return CmdErr::BadAction;
  }
  JsonVariantConst val = json["value"];
  if (val.isNull()) return CmdErr::Ok;
  if (val.is<bool>()) {
    out.hasValue = true;
    out.isBool = true;
    out.value = val.as<bool>() ? 1 : 0;
  } else if (val.is<double>()) {
    out.hasValue = true;
    out.value = val.as<double>();
  }  // a string or object leaves hasValue false, which checkCommand reports as bad_value
  return CmdErr::Ok;
}

}  // namespace espwf
