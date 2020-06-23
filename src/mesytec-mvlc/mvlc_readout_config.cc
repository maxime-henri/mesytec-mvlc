#include "mvlc_readout_config.h"

#include <cassert>
#include <yaml-cpp/yaml.h>
#include <yaml-cpp/emittermanip.h>

namespace mesytec
{
namespace mvlc
{

namespace
{
std::string to_string(ConnectionType ct)
{
    switch (ct)
    {
        case ConnectionType::USB:
            return listfile::get_filemagic_usb();

        case ConnectionType::ETH:
            return listfile::get_filemagic_eth();
    }

    return {};
}

ConnectionType connection_type_from_string(const std::string &str)
{
    if (str == listfile::get_filemagic_usb())
        return ConnectionType::USB;

    if (str == listfile::get_filemagic_eth())
        return ConnectionType::ETH;

    throw std::runtime_error("invalid connection type: " + str);
}

} // end anon namespace

bool CrateConfig::operator==(const CrateConfig &o) const
{
    return connectionType == o.connectionType
        && usbIndex == o.usbIndex
        && usbSerial == o.usbSerial
        && ethHost == o.ethHost
        && stacks == o.stacks
        && triggers == o.triggers
        && initCommands == o.initCommands
        && stopCommands == o.stopCommands
        && initTriggerIO == o.initTriggerIO
        ;
}

namespace
{

YAML::Emitter &operator<<(YAML::Emitter &out, const StackCommandBuilder &stack)
{
    out << YAML::BeginMap;

    out << YAML::Key << "name" << YAML::Value << stack.getName();

    out << YAML::Key << "groups" << YAML::BeginSeq;

    for (const auto &group: stack.getGroups())
    {
        out << YAML::BeginMap;
        out << YAML::Key << "name" << YAML::Value << group.name;

        out << YAML::Key << "contents" << YAML::Value;

        out << YAML::BeginSeq;
        for (const auto &cmd: group.commands)
            out << to_string(cmd);
        out << YAML::EndSeq;

        out << YAML::EndMap;
    }

    out << YAML::EndSeq; // groups

    out << YAML::EndMap;

    return out;
}

StackCommandBuilder stack_command_builder_from_yaml(const YAML::Node &yStack)
{
    StackCommandBuilder stack;

    stack.setName(yStack["name"].as<std::string>());

    if (const auto &yGroups = yStack["groups"])
    {
        for (const auto &yGroup: yGroups)
        {
            std::string groupName = yGroup["name"].as<std::string>();

            std::vector<StackCommand> groupCommands;

            for (const auto &yCmd: yGroup["contents"])
                groupCommands.emplace_back(stack_command_from_string(yCmd.as<std::string>()));

            stack.addGroup(groupName, groupCommands);
        }
    }

    return stack;
}

} // end anon namespace

std::string to_yaml(const CrateConfig &crateConfig)
{
    YAML::Emitter out;

    out << YAML::Hex;

    assert(out.good());

    out << YAML::BeginMap;
    out << YAML::Key << "crate" << YAML::Value << YAML::BeginMap;

    out << YAML::Key << "mvlc_connection" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "type" << YAML::Value << to_string(crateConfig.connectionType);
    out << YAML::Key << "usbIndex" << YAML::Value << std::to_string(crateConfig.usbIndex);
    out << YAML::Key << "usbSerial" << YAML::Value << crateConfig.usbSerial;
    out << YAML::Key << "ethHost" << YAML::Value << crateConfig.ethHost;
    out << YAML::Key << "ethJumboEnable" << YAML::Value << crateConfig.ethJumboEnable;
    out << YAML::EndMap; // end mvlc_connection

    out << YAML::Key << "readout_stacks" << YAML::Value << YAML::BeginSeq;

    for (const auto &stack: crateConfig.stacks)
        out << stack;

    out << YAML::EndSeq; // end readout_stacks

    out << YAML::Key << "stack_triggers" << YAML::Value << crateConfig.triggers;
    out << YAML::Key << "init_commands" << YAML::Value << crateConfig.initCommands;
    out << YAML::Key << "stop_commands" << YAML::Value << crateConfig.stopCommands;
    out << YAML::Key << "init_trigger_io" << YAML::Value << crateConfig.initTriggerIO;

    out << YAML::EndMap; // end crate

    assert(out.good());

    return out.c_str();
}

CrateConfig crate_config_from_yaml(const std::string &yamlText)
{
    std::istringstream iss(yamlText);
    return crate_config_from_yaml(iss);
}

CrateConfig crate_config_from_yaml(std::istream &input)
{
    CrateConfig result = {};

    YAML::Node yRoot = YAML::Load(input);

    if (!yRoot)
        throw std::runtime_error("CrateConfig YAML is empty");

    if (!yRoot["crate"])
        throw std::runtime_error("No 'crate' node found in YAML input");

    if (const auto &yCrate = yRoot["crate"])
    {
        if (const auto &yCon = yCrate["mvlc_connection"])
        {
            result.connectionType = connection_type_from_string(yCon["type"].as<std::string>());
            result.usbIndex = yCon["usbIndex"].as<int>();
            result.usbSerial = yCon["usbSerial"].as<std::string>();
            result.ethHost = yCon["ethHost"].as<std::string>();
            result.ethJumboEnable = yCon["ethJumboEnable"].as<bool>();
        }

        if (const auto &yStacks = yCrate["readout_stacks"])
        {
            for (const auto &yStack: yStacks)
                result.stacks.emplace_back(stack_command_builder_from_yaml(yStack));
        }

        if (const auto &yTriggers = yCrate["stack_triggers"])
        {
            for (const auto &yTrig: yTriggers)
                result.triggers.push_back(yTrig.as<u32>());
        }

        result.initCommands = stack_command_builder_from_yaml(yCrate["init_commands"]);
        result.stopCommands = stack_command_builder_from_yaml(yCrate["stop_commands"]);
        result.initTriggerIO = stack_command_builder_from_yaml(yCrate["init_trigger_io"]);
    }

    return result;
}

} // end namespace mvlc
} // end namespace mesytec
