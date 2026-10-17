#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Shared plumbing for the engine benchmarks in this directory: timing,
// statistics, command-line options and CSV output. Each benchmark only
// decides what to measure.
//
// Every benchmark writes two CSVs:
//   runs     one row per condition per repetition
//   summary  one row per condition: mean and sample standard deviation of
//            each metric over the repetitions
namespace Bench {

using Clock = std::chrono::steady_clock;

inline std::int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

inline double mean(const std::vector<double>& values)
{
    if (values.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (double value : values) {
        sum += value;
    }
    return sum / static_cast<double>(values.size());
}

inline double sampleStddev(const std::vector<double>& values)
{
    if (values.size() < 2) {
        return 0.0;
    }
    const double average = mean(values);
    double squares = 0.0;
    for (double value : values) {
        squares += (value - average) * (value - average);
    }
    return std::sqrt(squares / static_cast<double>(values.size() - 1));
}

// Nearest-rank percentile, p in [0, 100].
inline double percentile(std::vector<double> values, double p)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(values.size())));
    return values[std::min(values.size() - 1, rank == 0 ? 0 : rank - 1)];
}

// Written to so the optimizer cannot drop work whose result is otherwise unused.
inline volatile std::uint64_t sink = 0;

struct Options {
    bool quick = false;
    std::string runsPath;
    std::string summaryPath;
};

inline Options parseOptions(int argc, char** argv, const std::string& name, const std::string& description)
{
    Options options;
    options.runsPath = name + "-runs.csv";
    options.summaryPath = name + "-summary.csv";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--quick") {
            options.quick = true;
        } else if ((arg == "--runs" || arg == "--summary") && i + 1 < argc) {
            (arg == "--runs" ? options.runsPath : options.summaryPath) = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << name << " -- " << description << "\n\n"
                      << "  --quick           Smoke-test matrix, one repetition. Not evidence.\n"
                      << "  --runs PATH       Per-repetition CSV (default " << options.runsPath << ")\n"
                      << "  --summary PATH    Per-condition CSV (default " << options.summaryPath << ")\n";
            std::exit(0);
        } else {
            std::cerr << name << ": unknown argument " << arg << " (try --help)\n";
            std::exit(2);
        }
    }
    return options;
}

// Collects one row of metrics per condition and repetition, keeping
// conditions in the order they were first added.
class Results {
public:
    Results(std::vector<std::string> keyColumns, std::vector<std::string> metricColumns)
        : keyColumns_(std::move(keyColumns)), metricColumns_(std::move(metricColumns))
    {
    }

    void add(const std::vector<std::string>& key, int repetition, const std::vector<double>& metrics)
    {
        if (key.size() != keyColumns_.size() || metrics.size() != metricColumns_.size()) {
            throw std::logic_error("benchmark row does not match its columns");
        }
        if (rows_.find(key) == rows_.end()) {
            order_.push_back(key);
        }
        rows_[key].push_back(Row{repetition, metrics});
    }

    void write(const Options& options) const
    {
        std::ofstream runs(options.runsPath);
        std::ofstream summary(options.summaryPath);
        if (!runs || !summary) {
            throw std::runtime_error("cannot open " + options.runsPath + " or " + options.summaryPath);
        }
        runs << std::setprecision(10);
        summary << std::setprecision(10);

        runs << join(keyColumns_) << ",repetition," << join(metricColumns_) << '\n';
        summary << join(keyColumns_) << ",runs";
        for (const std::string& metric : metricColumns_) {
            summary << ',' << metric << "_mean," << metric << "_stddev";
        }
        summary << '\n';

        for (const auto& key : order_) {
            const std::vector<Row>& rows = rows_.at(key);
            for (const Row& row : rows) {
                runs << join(key) << ',' << row.repetition;
                for (double value : row.metrics) {
                    runs << ',' << value;
                }
                runs << '\n';
            }
            summary << join(key) << ',' << rows.size();
            for (std::size_t m = 0; m < metricColumns_.size(); ++m) {
                const std::vector<double> values = column(rows, m);
                summary << ',' << mean(values) << ',' << sampleStddev(values);
            }
            summary << '\n';
        }
    }

    // The summary means as an aligned table on stdout.
    void print() const
    {
        std::vector<std::string> header = keyColumns_;
        header.insert(header.end(), metricColumns_.begin(), metricColumns_.end());
        std::vector<std::vector<std::string>> table{header};
        for (const auto& key : order_) {
            std::vector<std::string> line = key;
            for (std::size_t m = 0; m < metricColumns_.size(); ++m) {
                std::ostringstream cell;
                cell << std::fixed << std::setprecision(3) << mean(column(rows_.at(key), m));
                line.push_back(cell.str());
            }
            table.push_back(line);
        }
        std::vector<std::size_t> widths(header.size(), 0);
        for (const auto& line : table) {
            for (std::size_t c = 0; c < line.size(); ++c) {
                widths[c] = std::max(widths[c], line[c].size());
            }
        }
        for (const auto& line : table) {
            for (std::size_t c = 0; c < line.size(); ++c) {
                std::cout << std::setw(static_cast<int>(widths[c]) + 2) << line[c];
            }
            std::cout << '\n';
        }
    }

private:
    struct Row {
        int repetition;
        std::vector<double> metrics;
    };

    static std::string join(const std::vector<std::string>& fields)
    {
        std::string out;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            out += (i == 0 ? "" : ",") + fields[i];
        }
        return out;
    }

    static std::vector<double> column(const std::vector<Row>& rows, std::size_t index)
    {
        std::vector<double> values;
        for (const Row& row : rows) {
            values.push_back(row.metrics[index]);
        }
        return values;
    }

    std::vector<std::string> keyColumns_;
    std::vector<std::string> metricColumns_;
    std::vector<std::vector<std::string>> order_;
    std::map<std::vector<std::string>, std::vector<Row>> rows_;
};

// Per-frame durations in nanoseconds -> mean, p95 and p99 in microseconds.
inline std::vector<double> frameStatsUs(const std::vector<double>& frameNs)
{
    return {mean(frameNs) / 1e3, percentile(frameNs, 95.0) / 1e3, percentile(frameNs, 99.0) / 1e3};
}

} // namespace Bench
