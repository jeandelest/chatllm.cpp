#include "chat.h"
#include <iomanip>
#include <iostream>
#include <fstream>
#include <thread>
#include <algorithm>
#include <numeric>
#include <functional>
#include <cstring>
#include <random>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

struct Args
{
    std::string model_path = "chatllm-ggml.bin";
    std::string system = "";
    std::string prompt = "你好";
    std::string extending = "restart";
    std::string test_fn = "";
    int max_length = -1;
    int max_context_length = 512;
    bool interactive = false;
    int top_k = 0;
    float top_p = 0.7;
    float temp = 0.7;
    int num_threads = 0;
    bool multi_line = false;
    int seed;
    chatllm::ChatFormat format = chatllm::ChatFormat::CHAT;
    bool tokenize = false;
};

#define MULTI_LINE_END_MARKER_W  L"\\."
#define MULTI_LINE_END_MARKER     "\\."

void usage(const char *prog)
{
    std::cout << "Usage: " << prog << " [options]\n"
              << "\n"
              << "options:\n"
              << "  -h, --help              show this help message and exit\n"
              << "  -m, --model PATH        model path (default: chatllm-ggml.bin)\n"
              << "  -p, --prompt PROMPT     prompt to start generation with (default: 你好)\n"
              << "  -s, --system SYSTEM     system prompt (instruction) (default: model specific)\n"
              << "  -i, --interactive       run in interactive mode\n"
              << "  -l, --max_length N      max total length including prompt and output (default: model specific)\n"
              << "  -t, --threads N         number of threads for inference (default: number of cores)\n"
              << "  -c, --max_context_length N\n"
              << "                          max context length (default: 512)\n"
              << "  --extending EXT         context extending method (EXT = restart | shift) (default: restart)\n"
              << "  --top_k N               top-k sampling (default: 0)\n"
              << "  --top_p N               top-p sampling (default: 0.7)\n"
              << "  --temp N                temperature (default: 0.95)\n"
              << "  --seed N                seed for random generator (default: random)\n"
              << "  --multi                 enabled multiple lines of input\n"
              << "                          when enabled,  `" << MULTI_LINE_END_MARKER << "` marks the end of your input.\n"
              << "  --format FMT            conversion format (model specific, FMT = chat | completion | qa) (default: chat)\n"
              << "  --tokenize              (debug)tokenize `prompt` and exit\n"
              << "  --test FILE             test again inputs from a file and exit\n"
              << std::endl;
}

static Args parse_args(int argc, const char **argv)
{
    Args args;
    std::random_device rd;
    args.seed = rd();

    #define handle_para0(fmt1, field, f)    \
        else if ((strcmp(arg, fmt1) == 0))      \
        {                                                                   \
            c++;                                                            \
            if (c < argc)                                                   \
                args.field = f(argv[c]);                                    \
        }

    #define handle_param(fmt1, fmt2, field, f)    \
        else if ((strcmp(arg, fmt1) == 0) || (strcmp(arg, fmt2) == 0))      \
        {                                                                   \
            c++;                                                            \
            if (c < argc)                                                   \
                args.field = f(argv[c]);                                    \
        }

    int c = 1;
    while (c < argc)
    {
        const char *arg = argv[c];
        if ((strcmp(arg, "--help") == 0) || (strcmp(arg, "-h") == 0))
        {
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
        else if ((strcmp(arg, "--interactive") == 0) || (strcmp(arg, "-i") == 0))
        {
            args.interactive = true;
        }
        else if (strcmp(arg, "--multi") == 0)
        {
            args.multi_line = true;
        }
        else if (strcmp(arg, "--tokenize") == 0)
        {
            args.tokenize = true;
        }
        else if (strcmp(arg, "--format") == 0)
        {
            c++;
            if (c < argc)
            {
                if (strcmp(argv[c], "completion") == 0)
                    args.format = chatllm::ChatFormat::COMPLETION;
                else if (strcmp(argv[c], "qa") == 0)
                    args.format = chatllm::ChatFormat::QA;
                else
                    args.format = chatllm::ChatFormat::CHAT;
            }
        }
        handle_param("--model",                 "-m", model_path,           std::string)
        handle_param("--prompt",                "-p", prompt,               std::string)
        handle_param("--system",                "-s", system,               std::string)
        handle_param("--max_length",            "-l", max_length,           std::stoi)
        handle_param("--max_context_length",    "-c", max_context_length,   std::stoi)
        handle_para0("--extending",                   extending,            std::string)
        handle_param("--top_k",                 "-k", top_k,                std::stoi)
        handle_param("--top_p",                 "-q", top_p,                std::stof)
        handle_param("--temp",                  "-t", temp,                 std::stof)
        handle_param("--threads",               "-n", num_threads,          std::stoi)
        handle_para0("--seed",                        seed,                 std::stoi)
        handle_para0("--test",                        test_fn,              std::string)
        else
            break;

        c++;
    }

    if (c < argc)
    {
        std::cerr << "Unknown arguments:";
        for (int i = c; i < argc; i++)
        {
            std::cerr << " " << argv[i];
        }
        std::cerr << std::endl;
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    return args;
}

#if defined(_WIN32)
static void append_utf8(char32_t ch, std::string &out)
{
    if (ch <= 0x7F)
    {
        out.push_back(static_cast<unsigned char>(ch));
    }
    else if (ch <= 0x7FF)
    {
        out.push_back(static_cast<unsigned char>(0xC0 | ((ch >> 6) & 0x1F)));
        out.push_back(static_cast<unsigned char>(0x80 | (ch & 0x3F)));
    }
    else if (ch <= 0xFFFF)
    {
        out.push_back(static_cast<unsigned char>(0xE0 | ((ch >> 12) & 0x0F)));
        out.push_back(static_cast<unsigned char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<unsigned char>(0x80 | (ch & 0x3F)));
    }
    else if (ch <= 0x10FFFF)
    {
        out.push_back(static_cast<unsigned char>(0xF0 | ((ch >> 18) & 0x07)));
        out.push_back(static_cast<unsigned char>(0x80 | ((ch >> 12) & 0x3F)));
        out.push_back(static_cast<unsigned char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<unsigned char>(0x80 | (ch & 0x3F)));
    }
    else
    {
        // Invalid Unicode code point
    }
}

static bool get_utf8_line(std::string &line, bool multi_line)
{
    std::wstring marker(MULTI_LINE_END_MARKER_W);

    do
    {
        std::wstring prompt;
        std::getline(std::wcin, prompt);

        if (multi_line)
        {
            if (prompt == marker)
                return true;
            if (line.size() > 0)
                append_utf8('\n', line);
        }

        for (auto wc : prompt)
            append_utf8(wc, line);
    } while (multi_line);

    return true;
}
#else
static bool get_utf8_line(std::string &line, bool multi_line)
{
    do
    {
        std::string prompt;
        std::getline(std::cin, prompt);

        if (multi_line)
        {
            if (prompt == MULTI_LINE_END_MARKER)
                return true;
            if (line.size() > 0)
                line.push_back('\n');
        }

        line.append(prompt.begin(), prompt.end());
    } while (multi_line);

    return true;
}
#endif

static inline int get_num_physical_cores()
{
    unsigned int n_threads = std::thread::hardware_concurrency();
    return n_threads > 0 ? (n_threads <= 4 ? n_threads : n_threads / 2) : 4;
}

static void trim(std::string &s)
{
    size_t l = s.size();
    while (l > 0)
    {
        if ((s[l - 1] == '\r') || (s[l - 1] == '\n'))
            l--;
        else
            break;
    }
    s.resize(l);
}

static void run_file(Args &args, chatllm::Pipeline &pipeline, chatllm::TextStreamer &streamer, const chatllm::GenerationConfig &gen_config)
{
    std::vector<std::string> history;
    std::string input;
    std::ifstream f(args.test_fn);

    if (f.is_open())
    {
        while (std::getline(f, input))
        {
            trim(input);
            std::cout << "You  > " << input << std::endl;
            history.emplace_back(std::move(input));

            std::cout << "A.I. > " << std::flush;
            std::string output = pipeline.chat(history, gen_config, &streamer);
            history.emplace_back(std::move(output));
        }
    }

    f.close();
    std::cout << std::endl << pipeline.model->get_n_past() << " tokens are processed/generated. Bye" << std::endl;
}

static void show_banner(chatllm::Pipeline &pipeline)
{
    #define MODEL_INFO()     "You are served by " << std::left << std::setw(28) << pipeline.model->type_name() + ","
    #define SHOW_NATIVE()    if (pipeline.model->native_name().size() > 0) { std::cout << "(" << pipeline.model->native_name() << ")"; }

    const int64_t total_param_num = pipeline.model->get_param_num(false);
    const int64_t total_effective_param_num = pipeline.model->get_param_num(true);

    std::cout   << R"(    ________          __  __    __    __  ___ )"; SHOW_NATIVE(); std::cout << '\n'
                << R"(   / ____/ /_  ____ _/ /_/ /   / /   /  |/  /_________  ____  )" << '\n'
                << R"(  / /   / __ \/ __ `/ __/ /   / /   / /|_/ // ___/ __ \/ __ \ )" << '\n'
                << R"( / /___/ / / / /_/ / /_/ /___/ /___/ /  / // /__/ /_/ / /_/ / )" << '\n'
                << R"( \____/_/ /_/\__,_/\__/_____/_____/_/  /_(_)___/ .___/ .___/  )" << '\n';
    std::cout   << MODEL_INFO()                               << R"(/_/   /_/       )" << '\n';
    if (total_param_num == total_effective_param_num)
        std::cout   << "with " << total_param_num << " (" << std::fixed << std::setprecision(1) << total_param_num / 1000000000. << "B) parameters." << '\n';
    else
        std::cout   << "with " << total_param_num << " (" << std::fixed << std::setprecision(1) << total_effective_param_num / 1000000000. << "B effect.) parameters." << '\n';

    std::cout << std::endl;
}

static void print_embedding(const std::vector<float> &data)
{
    for (size_t i = 0; i < data.size(); i++)
    {
        if ((i % 8) == 0) std::cout << std::endl;
        std::cout << std::fixed << std::setprecision(8) << data[i] << "  ";
    }
    std::cout << std::endl;
}

static void run_text_embedding(Args &args, chatllm::Pipeline &pipeline, chatllm::TextStreamer &streamer, const chatllm::GenerationConfig &gen_config)
{
    std::vector<float> result;

    if (!args.interactive)
    {
        pipeline.text_embedding(args.prompt, gen_config, result);
        print_embedding(result);
        return;
    }

    show_banner(pipeline);

    while (1)
    {
        std::cout << "Input > " << std::flush;
        std::string input;
        if (!get_utf8_line(input, args.multi_line))
        {
            std::cout << "FAILED to read line." << std::endl;
            break;
        }
        if (input.empty()) continue;

        result.clear();
        pipeline.text_embedding(input, gen_config, result);
        std::cout << "      > ";

        print_embedding(result);

    }
    std::cout << "Bye\n";
}

static void run_qa_ranker(Args &args, chatllm::Pipeline &pipeline, chatllm::TextStreamer &streamer, const chatllm::GenerationConfig &gen_config)
{

}

void chat(Args &args)
{
    chatllm::Pipeline pipeline(args.model_path);

    if (args.system.size() > 0)
        pipeline.set_system_prompt(args.system);
    pipeline.model->seed(args.seed);
    if (args.max_length < 0)
        args.max_length = pipeline.model->get_max_length();
    if (args.max_length > pipeline.model->get_max_length())
        args.max_length = pipeline.model->get_max_length();

    if (args.extending == "shift")
        pipeline.set_extending_method(chatllm::Pipeline::ExtendingMethod::Shift);
    else
        pipeline.set_extending_method(chatllm::Pipeline::ExtendingMethod::Restart);

    pipeline.tokenizer->set_chat_format(args.format);

    const std::string ai_prompt   = "A.I.";
    const std::string user_prompt = "You";
    const int prompt_len = 4;

    chatllm::TextStreamer streamer(pipeline.tokenizer.get());

    chatllm::GenerationConfig gen_config(args.max_length, args.max_context_length, args.temp > 0, args.top_k,
                                         args.top_p, args.temp, args.num_threads);

#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_WTEXT);
    // Set console code page to UTF-8 so console known how to interpret string data
    SetConsoleOutputCP(CP_UTF8);
    // Enable buffering to prevent VS from chopping up UTF-8 byte sequences
    setvbuf(stdout, nullptr, _IOFBF, 1000);
#endif

    if (args.tokenize)
    {
        auto ids = pipeline.tokenizer->encode(args.prompt);
        std::cout << "ID: ";
        for (auto x : ids)
            std::cout << x << ", ";
        std::cout << std::endl;
        return;
    }

    switch (pipeline.model->get_purpose())
    {
    case chatllm::ModelPurpose::TextEmbedding:
        run_text_embedding(args, pipeline, streamer, gen_config);
        return;
    case chatllm::ModelPurpose::Ranker:
        run_qa_ranker(args, pipeline, streamer, gen_config);
        return;
    default:
        break;
    }

    if (args.test_fn.size() > 0)
    {
        run_file(args, pipeline, streamer, gen_config);
        return;
    }

    if (!args.interactive)
    {
        pipeline.chat({args.prompt}, gen_config, &streamer);
        return;
    }

    show_banner(pipeline);

    std::vector<std::string> history;
    while (1)
    {
        std::cout << std::setw(prompt_len) << std::left << user_prompt << " > " << std::flush;
        std::string input;
        if (!get_utf8_line(input, args.multi_line))
        {
            std::cout << "FAILED to read line." << std::endl;
            break;
        }
        if (input.empty()) continue;

        history.emplace_back(std::move(input));
        std::cout << std::setw(prompt_len) << std::left << ai_prompt << " > " << std::flush;
        std::string output = pipeline.chat(history, gen_config, &streamer);
        history.emplace_back(std::move(output));
    }
    std::cout << "Bye\n";
}

int main(int argc, const char **argv)
{
    Args args = parse_args(argc, argv);
    if (args.num_threads <= 0)
        args.num_threads = get_num_physical_cores();

    try
    {
        chat(args);
    }
    catch (std::exception &e)
    {
        std::cerr << e.what() << std::endl;
        exit(EXIT_FAILURE);
    }
    return 0;
}
