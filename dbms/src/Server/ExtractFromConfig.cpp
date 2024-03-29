#include <iostream>

#include <boost/program_options.hpp>
#include <Poco/Logger.h>
#include <Poco/ConsoleChannel.h>
#include <Poco/FormattingChannel.h>
#include <Poco/PatternFormatter.h>
#include <Poco/AutoPtr.h>

#include <Common/Config/ConfigProcessor.h>
#include <Common/Exception.h>
#include <Common/Config/TOMLConfiguration.h>

static void setupLogging(const std::string & log_level)
{
    Poco::AutoPtr<Poco::ConsoleChannel> channel(new Poco::ConsoleChannel);
    Poco::AutoPtr<Poco::PatternFormatter> formatter(new Poco::PatternFormatter);
    formatter->setProperty("pattern", "%L%Y-%m-%d %H:%M:%S.%i <%p> %s: %t");
    Poco::AutoPtr<Poco::FormattingChannel> formatting_channel(new Poco::FormattingChannel(formatter, channel));
    Poco::Logger::root().setChannel(formatting_channel);
    Poco::Logger::root().setLevel(log_level);
}

static std::string extractFromConfig(
        const std::string & config_path, const std::string & key, bool try_get = false)
{
    ConfigProcessor processor(config_path, /* log_to_console = */ false);
    auto config_conf = processor.processConfig();
    ConfigurationPtr configuration(new DB::TOMLConfiguration(config_conf));
    // do not throw exception if not found
    if (try_get)
        return configuration->getString(key, "");
    return configuration->getString(key);
}

int mainEntryClickHouseExtractFromConfig(int argc, char ** argv)
{
    bool print_stacktrace = false;
    bool try_get = false;
    std::string log_level;
    std::string config_path;
    std::string key;

    namespace po = boost::program_options;

    po::options_description options_desc("Allowed options");
    options_desc.add_options()
        ("help", "produce this help message")
        ("stacktrace", po::bool_switch(&print_stacktrace), "print stack traces of exceptions")
        ("try", po::bool_switch(&try_get), "Do not warn about missing keys")
        ("log-level", po::value<std::string>(&log_level)->default_value("error"), "log level")
        ("config-file,c", po::value<std::string>(&config_path)->required(), "path to config file")
        ("key,k", po::value<std::string>(&key)->required(), "key to get value for");

    po::positional_options_description positional_desc;
    positional_desc.add("config-file", 1);
    positional_desc.add("key", 1);

    try
    {
        po::variables_map options;
        po::store(po::command_line_parser(argc, argv).options(options_desc).positional(positional_desc).run(), options);

        if (options.count("help"))
        {
            std::cerr << "Preprocess config file and extract value of the given key." << std::endl
                << std::endl;
            std::cerr << "Usage: clickhouse extract-from-config [options]" << std::endl
                << std::endl;
            std::cerr << options_desc << std::endl;
            return 0;
        }

        po::notify(options);

        setupLogging(log_level);
        std::cout << extractFromConfig(config_path, key, try_get) << std::endl;
    }
    catch (...)
    {
        std::cerr << DB::getCurrentExceptionMessage(print_stacktrace, true) << std::endl;
        return DB::getCurrentExceptionCode();
    }

    return 0;
}
