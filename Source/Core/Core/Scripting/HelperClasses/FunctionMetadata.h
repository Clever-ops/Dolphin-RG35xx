#pragma once
#include <string>
#include "Core/Scripting/HelperClasses/ArgHolder.h"
#include "Core/Scripting/HelperClasses/ArgTypeEnum.h"
#include "Core/Scripting/ScriptContext.h"

namespace Scripting
{
struct FunctionMetadata
{
  std::string function_name;
  std::string function_version;
  std::string example_function_call;
  ArgHolder (*function_pointer)(ScriptContext*, std::vector<ArgHolder>&);
  ArgTypeEnum return_type;
  std::vector<ArgTypeEnum> arguments_list;
};
}  // namespace Scripting