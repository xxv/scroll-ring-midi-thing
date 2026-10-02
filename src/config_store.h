// JSON (de)serialisation of mappings/settings and persistence to LittleFS.
#pragma once

#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "engine.h"

void mappingToJson(const Mapping& m, JsonObject out);
// Fields missing from `in` keep their current value in `m`.
bool mappingFromJson(JsonObjectConst in, Mapping& m, std::string& err);

void settingsToJson(const Settings& s, JsonObject out);
void settingsFromJson(JsonObjectConst in, Settings& s);

std::string configToJson(const Engine& engine);
bool configFromJson(const std::string& json, std::vector<Mapping>& mappings, Settings& settings, std::string& err);

bool storageBegin();
bool storageSave(const std::string& json);
std::string storageLoad();
