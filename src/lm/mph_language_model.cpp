/**
 * @file mph_language_model.cpp
 * @author Chase Geigle
 *
 * All files in META are dual-licensed under the MIT and NCSA licenses. For more
 * details, consult the file LICENSE.mit and LICENSE.ncsa in the root of the
 * project.
 */

#include <cassert>

#include "meta/io/filesystem.h"
#include "meta/lm/mph_language_model.h"
#include "meta/lm/ngram_map.h"
#include "meta/util/algorithm.h"
#include "meta/util/shim.h"
#include "meta/util/time.h"

namespace meta
{
namespace lm
{

namespace
{
template <class NGramHandler>
void read_from_arpa(const std::string& filename, NGramHandler&& ngram_handler)
{
    std::ifstream infile{filename};
    std::string buffer;

    // get to beginning of unigram data, observing the counts of each ngram type
    while (std::getline(infile, buffer))
    {
        if (buffer.find("ngram ") == 0)
        {
            auto equal = buffer.find_first_of("=");
            ngram_handler.count(std::stoul(buffer.substr(equal + 1)));
        }

        if (buffer.find("\\1-grams:") == 0)
            break;
    }

    uint64_t order = 0;
    while (std::getline(infile, buffer))
    {
        // if blank or end
        if (buffer.empty() || (buffer[0] == '\\' && buffer[1] == 'e'))
            continue;

        // if start of new ngram data
        if (buffer[0] == '\\')
        {
            ++order;
            continue;
        }

        auto first_tab = buffer.find_first_of('\t');
        float prob = std::stof(buffer.substr(0, first_tab));
        auto second_tab = buffer.find_first_of('\t', first_tab + 1);
        auto ngram = buffer.substr(first_tab + 1, second_tab - first_tab - 1);
        float backoff = 0.0;
        if (second_tab != std::string::npos)
            backoff = std::stof(buffer.substr(second_tab + 1));

        ngram_handler(order, ngram, prob, backoff);
    }
}

class ngram_handler
{
  public:
    using unigram_builder_type = ngram_map_builder<std::string>;
    using middle_builder_type = ngram_map_builder<std::vector<uint64_t>>;
    using last_builder_type = ngram_map_builder<std::vector<uint64_t>, float>;

    ngram_handler(const std::string& prefix) : prefix_{prefix}
    {
        // nothing
    }

    void count(uint64_t ngram_count)
    {
        counts_.emplace_back(ngram_count);
        LOG(info) << counts_.size() << "-gram count: " << ngram_count << ENDLG;
        if (counts_.size() == 1)
        {
            unigram_builder_type::options_type options;
            options.prefix = prefix_ + "/0/";
            options.num_keys = ngram_count;

            filesystem::make_directory(options.prefix);

            unigram_builder_ = make_unique<unigram_builder_type>(options);
        }
    }

    void operator()(uint64_t order, const std::string& ngram, float prob,
                    float backoff)
    {
        if (order > order_)
        {
            finish_order();
            order_ = order;
        }

        ++observed_;
        if (observed_ > counts_[order])
            throw std::runtime_error{"too many " + std::to_string(order + 1)
                                     + "-grams"};

        if (order_ == 0)
        {
            assert(unigram_builder_);
            prob_backoff<> value;
            value.prob = prob;
            value.backoff = backoff;
            (*unigram_builder_)(ngram, value);
        }
        else
        {
            assert(middle_builder_ || last_builder_);
            assert(unigrams_);

            std::vector<uint64_t> ids;
            ids.reserve(order);

            util::for_each_token(
                ngram.begin(), ngram.end(), " ",
                [&](std::string::const_iterator first,
                    std::string::const_iterator last)
                {
                    if (first != last)
                    {
                        std::string unigram{first, last};
                        auto id = unigrams_->index({first, last});
                        if (!id)
                            throw std::runtime_error{
                                "ngram contains unknown unigram " + unigram};
                        ids.push_back(*id);
                    }
                });

            assert(ids.size() == order + 1);

            prob_backoff<> value;
            value.prob = prob;
            value.backoff = backoff;

            if (order_ != counts_.size() - 1)
                (*middle_builder_)(ids, value);
            else
                (*last_builder_)(ids, prob);
        }
    }

    uint64_t order() const
    {
        return order_;
    }

    void finish_order()
    {
        LOG(info) << "Finalizing " << order_ + 1 << "-grams (" << observed_
                  << ")" << ENDLG;
        observed_ = 0;
        // unigrams
        if (order_ == 0)
        {
            assert(unigram_builder_);
            unigram_builder_->write();
            unigram_builder_ = nullptr;

            // now that the unigrams are written, we're going to load their
            // ngram_map to use as a vocabulary lookup
            unigrams_ = make_unique<ngram_map<std::string>>(prefix_ + "/0");
            LOG(info) << "Loaded unigram map" << ENDLG;

            middle_builder_type::options_type options;
            options.prefix = prefix_ + "/" + std::to_string(order_ + 1);
            options.num_keys = counts_[order_ + 1];

            filesystem::make_directory(options.prefix);

            middle_builder_ = make_unique<middle_builder_type>(options);
        }
        // middle
        else if (order_ < counts_.size() - 1)
        {
            assert(middle_builder_);
            middle_builder_->write();

            // set up the next middle builder
            if (order_ + 1 < counts_.size() - 1)
            {
                middle_builder_type::options_type options;
                options.prefix = prefix_ + "/" + std::to_string(order_ + 1);
                options.num_keys = counts_[order_ + 1];

                filesystem::make_directory(options.prefix);

                middle_builder_ = make_unique<middle_builder_type>(options);
            }
            // here come the final ngrams
            else
            {
                middle_builder_ = nullptr;

                last_builder_type::options_type options;
                options.prefix = prefix_ + "/" + std::to_string(order_ + 1);
                options.num_keys = counts_[order_ + 1];

                filesystem::make_directory(options.prefix);

                last_builder_ = make_unique<last_builder_type>(options);
            }
        }
        // last
        else
        {
            assert(last_builder_);
            last_builder_->write();
            last_builder_ = nullptr;
        }
    }

  private:
    std::string prefix_;
    uint64_t order_ = 0;
    uint64_t observed_ = 0;
    std::vector<uint64_t> counts_;
    std::unique_ptr<unigram_builder_type> unigram_builder_;
    std::unique_ptr<ngram_map<std::string>> unigrams_;
    std::unique_ptr<middle_builder_type> middle_builder_;
    std::unique_ptr<last_builder_type> last_builder_;
};

uint64_t build_from_arpa(const std::string& arpa_file,
                         const std::string& prefix)
{
    filesystem::remove_all(prefix);
    filesystem::make_directory(prefix);
    ngram_handler handler{prefix};
    read_from_arpa(arpa_file, handler);

    // finish off the n-grams
    handler.finish_order();

    return handler.order();
}
}

mph_language_model::mph_language_model(const cpptoml::table& config)
{
    auto table = config.get_table("mph-language-model");
    auto arpa_file = table->get_as<std::string>("arpa-file");
    auto prefix = table->get_as<std::string>("binary-file-prefix");

    LOG(info) << "Building language model from .arpa file: " << *arpa_file
              << ENDLG;

    uint64_t order;
    auto time = common::time([&]()
                             {
                                 order = build_from_arpa(*arpa_file, *prefix);
                             });
    LOG(info) << "Done. (" << time.count() << "ms)" << ENDLG;
}
}
}