//
// Sophos - Forward Private Searchable Encryption
// Copyright (C) 2016 Raphael Bost
//
// This file is part of Sophos.
//
// Sophos is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// Sophos is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with Sophos.  If not, see <http://www.gnu.org/licenses/>.
//


#pragma once

#include <sse/schemes/diana/diana_common.hpp>
#include <sse/schemes/diana/token_tree.hpp>
#include <sse/schemes/diana/types.hpp>
#include <sse/schemes/utils/rocksdb_wrapper.hpp>
#include <sse/schemes/utils/thread_pool.hpp>

#include <sse/crypto/prf.hpp>

#define MIN(a, b) (((a) > (b)) ? (b) : (a))

namespace sse {
namespace diana {


template<typename T>
class DianaServer
{
public:
    static constexpr size_t kKeySize = 32;

    using index_type = T;

    explicit DianaServer(const std::string& db_path);

    std::list<index_type> search(SearchRequest& req,
                                 bool           delete_results = false);
    void                  search(SearchRequest&                         req,
                                 const std::function<void(index_type)>& post_callback,
                                 bool                                   delete_results = false);
    void                  search_simple(SearchRequest&                         req,
                                        const std::function<void(index_type)>& post_callback,
                                        bool delete_results = false);

    std::list<index_type> search_simple_parallel(SearchRequest& req,
                                                 uint8_t        threads_count,
                                                 bool delete_results = false);
    void                  search_simple_parallel(SearchRequest&           req,
                                                 uint8_t                  threads_count,
                                                 std::vector<index_type>& results,
                                                 bool delete_results = false);
    void                  search_simple_parallel(
                         SearchRequest&                         req,
                         const std::function<void(index_type)>& post_callback,
                         uint8_t                                threads_count,
                         bool                                   delete_results = false);
    void search_simple_parallel(
        SearchRequest&                                  req,
        const std::function<void(index_type, uint8_t)>& post_callback,
        uint8_t                                         threads_count,
        bool                                            delete_results = false);


    void update(const UpdateRequest<index_type>& req);

    std::ostream& print_stats(std::ostream& out) const;

    void flush_edb();

private:
    bool get_unmask(uint8_t* key, index_type& index, bool delete_key);

    inline bool retrieve_entry(const update_token_type& key,
                               index_type&              index,
                               bool                     delete_key)
    {
        bool found = edb_.get(key, index);
        if (delete_key && found) {
            edb_.remove(key);
        }

        return found;
    }


    sophos::RockDBWrapper edb_;
};

} // namespace diana
} // namespace sse

namespace sse {
namespace diana {

template<typename T>
DianaServer<T>::DianaServer(const std::string& db_path) : edb_(db_path)
{
}

template<typename T>
bool DianaServer<T>::get_unmask(uint8_t*    key,
                                index_type& index,
                                bool        delete_key)
{
    update_token_type ut;
    index_type        mask;

    if (logger::severity() <= logger::LoggerSeverity::DBG) {
        logger::log(logger::LoggerSeverity::DBG)
            << "Derived leaf token: "
            << utility::hex_string(std::string(
                   reinterpret_cast<const char*>(key), kSearchTokenKeySize))
            << std::endl;
    }

    gen_update_token_mask<T>(key, ut, mask);


    if (logger::severity() <= logger::LoggerSeverity::DBG) {
        logger::log(logger::LoggerSeverity::DBG)
            << "Derived token : " << utility::hex_string(ut) << std::endl;
        logger::log(logger::LoggerSeverity::DBG)
            << "Mask : " << utility::hex_string(mask) << std::endl;
    }

    bool found = retrieve_entry(ut, index, delete_key);

    if (found) {
        if (logger::severity() <= logger::LoggerSeverity::DBG) {
            logger::log(logger::LoggerSeverity::DBG)
                << "Found: " << utility::hex_string(index) << std::endl;
        }

        index = xor_mask(index, mask);
    } else {
        logger::log(logger::LoggerSeverity::ERROR)
            << "We were supposed to find something!" << std::endl;
    }

    return found;
}

template<typename T>
std::list<typename DianaServer<T>::index_type> DianaServer<T>::search(
    SearchRequest& req,
    bool           delete_results)
{
    std::list<index_type> results;

    auto callback = [&results](index_type i) { results.push_back(i); };

    search(req, callback, delete_results);

    return results;
}

template<typename T>
void DianaServer<T>::search(
    SearchRequest&                         req,
    const std::function<void(index_type)>& post_callback,
    bool                                   delete_results)
{
    if (logger::severity() <= logger::LoggerSeverity::DBG) {
        logger::log(logger::LoggerSeverity::DBG)
            << "Expected matches: " << req.add_count << std::endl;
        logger::log(logger::LoggerSeverity::DBG)
            << "Number of search nodes: " << req.token_list.size() << std::endl;
    }

    crypto::Prf<kUpdateTokenSize> derivation_prf(
        crypto::Key<kKeySize>(req.kw_token.data()));

    // cppcheck-suppress variableScope
    auto get_callback = [this, &post_callback, delete_results](uint8_t* key) {
        index_type index;
        if (get_unmask(key, index, delete_results)) {
            post_callback(index);
        }
    };

    for (auto& it_token : req.token_list) {
        if (logger::severity() <= logger::LoggerSeverity::DBG) {
            logger::log(logger::LoggerSeverity::DBG)
                << "Search token key: " << utility::hex_string(it_token.first)
                << std::endl;
            logger::log(logger::LoggerSeverity::DBG)
                << "Search token depth: " << std::dec
                << static_cast<uint32_t>(it_token.second) << std::endl;
        }

        TokenTree::derive_all_leaves(
            it_token.first, it_token.second, get_callback);
    }
}

template<typename T>
void DianaServer<T>::search_simple(
    SearchRequest&                         req,
    const std::function<void(index_type)>& post_callback,
    bool                                   delete_results)
{
    index_type r;

    if (logger::severity() <= logger::LoggerSeverity::DBG) {
        logger::log(logger::LoggerSeverity::DBG)
            << "Expected matches: " << req.add_count << std::endl;
        logger::log(logger::LoggerSeverity::DBG)
            << "Number of search nodes: " << req.token_list.size() << std::endl;
    }

    crypto::Prf<kUpdateTokenSize> derivation_prf(
        crypto::Key<kKeySize>(req.kw_token.data()));

    for (auto& it_token : req.token_list) {
        if (logger::severity() <= logger::LoggerSeverity::DBG) {
            logger::log(logger::LoggerSeverity::DBG)
                << "Search token key: " << utility::hex_string(it_token.first)
                << std::endl;
            logger::log(logger::LoggerSeverity::DBG)
                << "Search token depth: " << std::dec
                << static_cast<uint32_t>(it_token.second) << std::endl;
        }

        // for now we implement the search algorithm in a naive way:
        // the tokens are iteratively generated using the derive_node function
        // this is not smart as some inner nodes will be recomputed several
        // times. we leave optimizations for later


        uint64_t count = 1 << it_token.second;

        for (uint64_t i = 0; i < count; i++) {
            // because we are using the naive algorithm, we will call
            // derive_node many times with the same key if not copied, the key
            // will be set to 0 during the first call so, although this is
            // unsecure, we have to explicitely copy the key
            auto node_copy = it_token.first;
            auto t         = TokenTree::derive_node(
                crypto::Key<TokenTree::kTokenSize>(node_copy.data()),
                i,
                it_token.second);

            if (logger::severity() <= logger::LoggerSeverity::DBG) {
                logger::log(logger::LoggerSeverity::DBG)
                    << "Derived leaf token: " << utility::hex_string(t)
                    << std::endl;
            }

            update_token_type ut;
            index_type        mask;

            gen_update_token_mask(t, ut, mask);

            if (logger::severity() <= logger::LoggerSeverity::DBG) {
                logger::log(logger::LoggerSeverity::DBG)
                    << "Derived token : " << utility::hex_string(ut)
                    << std::endl;
                logger::log(logger::LoggerSeverity::DBG)
                    << "Mask : " << std::hex << mask << std::endl;
            }

            bool found = retrieve_entry(ut, r, delete_results);

            if (found) {
                if (logger::severity() <= logger::LoggerSeverity::DBG) {
                    logger::log(logger::LoggerSeverity::DBG)
                        << "Found: " << std::hex << r << std::endl;
                }

                r ^= mask;

                if (logger::severity() <= logger::LoggerSeverity::DBG) {
                    logger::log(logger::LoggerSeverity::DBG)
                        << "Unmasked: " << std::hex << r << std::endl;
                }

                post_callback(r);
            } else {
                logger::log(logger::LoggerSeverity::ERROR)
                    << "We were supposed to find something!" << std::endl;
            }
        }
    }
}

template<typename T>
std::list<typename DianaServer<T>::index_type> DianaServer<
    T>::search_simple_parallel(SearchRequest& req,
                               uint8_t        threads_count,
                               bool           delete_results)
{
    assert(threads_count > 0);


    // use one result list per thread so to avoid using locks
    std::list<index_type>* result_lists
        = new std::list<index_type>[threads_count];

    auto callback = [&result_lists](index_type i, uint8_t thread_id) {
        result_lists[thread_id].push_back(i);
    };

    search_simple_parallel(req, callback, threads_count, delete_results);

    // merge the result lists
    std::list<index_type> results(std::move(result_lists[0]));
    for (uint8_t i = 1; i < threads_count; i++) {
        results.splice(results.end(), result_lists[i]);
    }

    delete[] result_lists;

    return results;
}

template<typename T>
void DianaServer<T>::search_simple_parallel(SearchRequest& req,
                                            uint8_t        threads_count,
                                            std::vector<index_type>& results,
                                            bool delete_results)
{
    if (results.size() < req.add_count) {
        // resize the vector if needed
        results.reserve(req.add_count);
    }

    std::atomic<uint64_t> r_index(0);

    auto callback = [&results, &r_index](index_type i, uint8_t /*thread_id*/) {
        results[r_index++] = i;
    };

    search_simple_parallel(req, callback, threads_count, delete_results);
}

template<typename T>
void DianaServer<T>::search_simple_parallel(
    SearchRequest&                         req,
    const std::function<void(index_type)>& post_callback,
    uint8_t                                threads_count,
    bool                                   delete_results)
{
    auto aux = [&post_callback](index_type ind, uint8_t /*i*/) {
        post_callback(ind);
    };
    search_simple_parallel(req, aux, threads_count, delete_results);
}

template<typename T>
void DianaServer<T>::search_simple_parallel(
    SearchRequest&                                  req,
    const std::function<void(index_type, uint8_t)>& post_callback,
    uint8_t                                         threads_count,
    bool                                            delete_results)
{
    assert(threads_count > 0);
    if (req.add_count == 0) {
        return;
    }

    auto job = [this, &post_callback, delete_results](
                   const uint8_t        t_id,
                   const SearchRequest& req,
                   const uint64_t       min_index,
                   const uint64_t       max_index) {
        auto get_callback
            // cppcheck-suppress variableScope
            = [this, t_id, &post_callback, delete_results](uint8_t* key) {
                  index_type index;
                  if (get_unmask(key, index, delete_results)) {
                      post_callback(index, t_id);
                  }
              };

        uint64_t loc_min_index = min_index;
        uint64_t loc_max_index = max_index;


        auto key_it = req.token_list.begin();

        do {
            // find the starting token
            // this is the number of leafs for the current node
            uint64_t leaf_count = (1UL << key_it->second);

            if ((leaf_count <= loc_min_index)) {
                // the selected leaf does not cover the minimum index
                // get the next node

                // update the local index counters
                loc_min_index -= leaf_count; // no underflow:
                loc_max_index -= leaf_count; // leaf_count <= loc_min_index <=
                                             // loc_max_index

            } else if ((leaf_count > loc_max_index)) {
                // this is the last node for us
                auto token = key_it->first; // copy the node as it will be
                                            // erased by the next function
                TokenTree::derive_leaves(token,
                                         key_it->second,
                                         loc_min_index,
                                         loc_max_index,
                                         get_callback);


                break;
            } else {
                // leaf_count > loc_min_index and leaf_count <= loc_max_index


                auto token = key_it->first; // copy the node as it will be
                                            // erased by the next function
                TokenTree::derive_leaves(token,
                                         key_it->second,
                                         loc_min_index,
                                         leaf_count - 1,
                                         get_callback);

                // update the local index counters
                loc_min_index = 0; // the first leaves have been generated now
                loc_max_index -= leaf_count; // leaf_count <= loc_min_index <=
                                             // loc_max_index
            }


            // get the next tree node
            ++key_it;
        } while ((key_it != req.token_list.end()));
    };

    std::vector<std::thread> threads;

    threads_count = MIN(threads_count, req.add_count);


    size_t step      = req.add_count / threads_count;
    size_t remaining = req.add_count % threads_count;

    size_t min = 0;
    size_t max = step;

    for (uint8_t t = 0; t < threads_count; t++) {
        if (t < remaining) {
            max++;
        }

        threads.push_back(
            std::thread(job, t, req, min, MIN(max, req.add_count) - 1));

        min = max;
        max += step;
    }

    for (uint8_t t = 0; t < threads_count; t++) {
        threads[t].join();
    }
}

template<typename T>
void DianaServer<T>::update(const UpdateRequest<T>& req)
{
    if (logger::severity() <= logger::LoggerSeverity::DBG) {
        logger::log(logger::LoggerSeverity::DBG)
            << "Update: (" << utility::hex_string(req.token) << ", "
            << utility::hex_string(req.index) << ")" << std::endl;
    }

    edb_.put(req.token, req.index);
}

template<typename T>
std::ostream& DianaServer<T>::print_stats(std::ostream& out) const
{
    return out;
}

template<typename T>
void DianaServer<T>::flush_edb()
{
    edb_.flush();
}
} // namespace diana
} // namespace sse
