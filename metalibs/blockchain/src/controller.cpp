#include "controller.hpp"

#include <blockchain.h>
#include <meta_log.hpp>
#include <open_ssl_decor.h>
#include <statics.hpp>

#include "block.h"
#include "chain.h"

#include <experimental/filesystem>
#include <fstream>
#include <list>

namespace fs = std::experimental::filesystem;
std::set<std::string> get_files_in_dir(std::string& path)
{
    std::set<std::string> files;

    //                          y   m   d   t
    const uint file_name_size = 4 + 2 + 2 + 4;

    for (const auto& p : fs::directory_iterator(path)) {
        const auto& path = p.path();
        auto filename = path.filename().string();

        if (filename.length() == file_name_size &&
            // Year
            std::isdigit(filename[0]) && std::isdigit(filename[1]) && std::isdigit(filename[2]) && std::isdigit(filename[3]) &&
            // Month
            std::isdigit(filename[4]) && std::isdigit(filename[5]) &&
            // Day
            std::isdigit(filename[6]) && std::isdigit(filename[7]) &&
            // Extension
            filename.compare(8, 4, std::string{ ".blk" }) == 0) {
            //            std::cout << path.string() << std::endl;
            files.insert(path.string());
        }
    }

    return files;
}

void parse_block_async(
    ThreadPool& TP,
    std::atomic<long int>& jobs,
    char* block_buff,
    int64_t block_size,
    std::mutex& block_lock,
    std::map<sha256_2, Block*>& block_tree,
    std::map<sha256_2, Block*>& prev_tree,
    bool delete_buff)
{
    TP.runAsync([&jobs, block_buff, block_size, &block_lock, &block_tree, &prev_tree, delete_buff]() {
        std::string_view block_as_string(block_buff, block_size);
        Block* block = parse_block(block_as_string);

        if (block) {
            std::lock_guard<std::mutex> lock(block_lock);
            if (!block_tree.insert({ block->get_block_hash(), block }).second) {
                DEBUG_COUT("Duplicate block in chain\t" + bin2hex(block->get_block_hash()));
                delete block;
            } else if (!prev_tree.insert({ block->get_prev_hash(), block }).second) {
                DEBUG_COUT("Branches in block chain\t" + bin2hex(block->get_prev_hash()) + "\t->\t" + bin2hex(block->get_block_hash()));
                block_tree.erase(block->get_block_hash());
                delete block;
            }
        } else {
            DEBUG_COUT("Block parse error");
        }

        if (delete_buff) {
            delete[] block_buff;
        }
        jobs--;
    });
}

ControllerImplementation::ControllerImplementation(
    const std::string& priv_key_line,
    const std::string _path,
    const std::string& proved_hash,
    const std::set<std::pair<std::string, int>>& core_list,
    const std::pair<std::string, int> host_port)
    : path(std::move(_path))
    , BC(new BlockChain())
    , cores(core_list, host_port)
{
    std::this_thread::sleep_for(std::chrono::seconds(2));
    {
        std::vector<unsigned char> priv_k = hex2bin(priv_key_line);
        PrivKey.insert(PrivKey.end(), priv_k.begin(), priv_k.end());
        if (!generate_public_key(PubKey, PrivKey)) {
            DEBUG_COUT("ERROR INVALID PRIVATE KEY");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            exit(1);
        }

        auto bin_addres = get_address(PubKey);
        Addres = "0x" + bin2hex(bin_addres);

        DEBUG_COUT("Core address is\t" + Addres);
    }

    {
        std::vector<unsigned char> bin_proved_hash = hex2bin(proved_hash);
        std::copy_n(bin_proved_hash.begin(), 32, proved_block.begin());
    }

    if (Addres == "0x00a88a888d16a23991e73b4081b745eec0f56cdc7063baa360") {
        master = true;
    } else {
        master = false;
    }

    read_and_apply_local_chain();

    start_main_loop();
}

void ControllerImplementation::read_and_apply_local_chain()
{
    std::mutex block_lock;
    std::atomic<long int> jobs(0);

    std::map<sha256_2, Block*> block_tree;
    std::map<sha256_2, Block*> prev_tree;

    char uint64_buff[8];
    std::set<std::string> files = get_files_in_dir(path);

    ThreadPool TP;

    for (const std::string& file : files) {
        std::ifstream ifile(file.c_str(), std::ios::in | std::ios::binary);

        if (ifile.is_open()) {
            while (ifile.read(uint64_buff, 8)) {
                uint64_t block_size = *(reinterpret_cast<uint64_t*>(uint64_buff));

                //                DEBUG_COUT("block_size:\t" + std::to_string(block_size));

                char* block_buff = new char[block_size];

                if (ifile.read(block_buff, static_cast<int64_t>(block_size))) {
                    jobs++;

                    parse_block_async(TP, jobs, block_buff, block_size, block_lock, block_tree, prev_tree, true);

                } else {
                    std::string msg = "read file error\t" + file;
                    DEBUG_COUT(msg);
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    exit(1);
                }
            }
        } else {
            std::string msg = "!file.is_open()\t" + file;
            DEBUG_COUT(msg);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            exit(1);
        }
    }

    while (jobs.load(std::memory_order_acquire) != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    DEBUG_COUT("PARSE BLOCKCHAIN COMPLETE");

    apply_block_chain(block_tree, prev_tree, "local storage", false);

    DEBUG_COUT("READ BLOCK COMPLETE");
}

void ControllerImplementation::apply_block_chain(std::map<sha256_2, Block*>& block_tree, std::map<sha256_2, Block*>& prev_tree, const std::string& source, bool need_write)
{

    sha256_2 zero_block = { { 0 } };
    sha256_2 that_one_block = last_applyed_block != zero_block ? last_applyed_block : proved_block;

    if (block_tree.find(that_one_block) == block_tree.end() && proved_block != zero_block) {
        for (auto& map_pair : block_tree) {
            delete map_pair.second;
        }
        DEBUG_COUT("blockchain from " + source + " have no blocks to connect with my chain");
        return;
    }

    std::list<Block*> block_chain;

    if (last_applyed_block == zero_block && proved_block != zero_block) {
        sha256_2 curr_block = proved_block;

        bool got_start = false;
        while (block_tree.find(curr_block) != block_tree.end()) {
            Block* block = block_tree[curr_block];
            block_chain.push_front(block);
            if (block->get_block_type() == BLOCK_TYPE_STATE || block->get_prev_hash() == zero_block) {
                got_start = true;
                break;
            }
            curr_block = block->get_prev_hash();
        }

        if (!got_start) {
            for (auto& map_pair : block_tree) {
                delete map_pair.second;
            }
            DEBUG_COUT("blockchain from " + source + " is incomplete");
            return;
        }
    }

    {
        sha256_2 curr_block = that_one_block;

        while (prev_tree.find(curr_block) != prev_tree.end()) {
            Block* block = prev_tree[curr_block];
            block_chain.push_back(block);
            curr_block = block->get_block_hash();
        }
    }

    for (Block* block : block_chain) {
        if (!BC->apply_block(block)) {
            DEBUG_COUT("blockchain from " + source + " have have errors in block " + bin2hex(block->get_block_hash()));
            break;
        }

        if (need_write) {
            {
                auto* p_ar = new ApproveRecord;
                p_ar->make(block->get_block_hash(), PrivKey, PubKey);
                p_ar->approve = true;
                distribute(p_ar);
                delete p_ar;
            }

            write_block(block);
        }

        prev_timestamp = block->get_block_timestamp();
        if (block->get_block_type() == BLOCK_TYPE_STATE) {
            prev_day = prev_timestamp / DAY_IN_SECONDS;
        }

        last_applyed_block = block->get_block_hash();
        last_created_block = block->get_block_hash();
        blocks.insert({ last_applyed_block, block });

        block->clean();
    }

    DEBUG_COUT("START");
    // {
    //     std::atomic<int> jobs = 0;
    //     for (auto lh_block_pair : block_tree) {
    //         jobs++;
    //         TP.runAsync([lh_block_pair, this, &jobs]() {
    //             bool is_in = false;
    //             for (auto rh_block_pair : blocks) {
    //                 if (lh_block_pair.second == rh_block_pair.second) {
    //                     is_in = true;
    //                     break;
    //                 }
    //             }
    //             if (!is_in) {
    //                 delete lh_block_pair.second;
    //             }
    //             jobs--;
    //         });
    //     }
    //     while (jobs.load()) {
    //         std::this_thread::sleep_for(std::chrono::milliseconds(1));
    //     }
    // }
    DEBUG_COUT("STOP");
}

void ControllerImplementation::actualize_chain()
{
    if (master) {
        return;
    }

    // DEBUG_COUT("actualize_chain");

    std::map<std::string, sha256_2> cores_with_missing_block;
    std::map<std::string, std::string> last_block_on_cores = cores.send_with_return(RPC_LAST_BLOCK, "");
    for (auto last_block_on_core : last_block_on_cores) {
        if (last_block_on_core.second.size() >= 40) {

            sha256_2 last_block_return;
            std::copy_n(last_block_on_core.second.begin(), 32, last_block_return.begin());

            uint64_t block_timestamp = 0;
            std::copy_n(last_block_on_core.second.begin() + 32, 8, reinterpret_cast<char*>(&block_timestamp));

            if (block_timestamp > prev_timestamp && blocks.find(last_block_return) == blocks.end()) {
                cores_with_missing_block.insert({ last_block_on_core.first, last_block_return });
            }
        }
    }

    for (const auto& core_block : cores_with_missing_block) {
        if (blocks.find(core_block.second) == blocks.end()) {
            std::string last_block_as_sting;
            std::copy_n(last_applyed_block.begin(), 32, std::back_inserter(last_block_as_sting));
            std::string return_data = cores.send_with_return_to_core(core_block.first, RPC_GET_CHAIN, last_block_as_sting);

            DEBUG_COUT("PARSE BLOCKCHAIN START");
            DEBUG_COUT("return_data.size()\t" + std::to_string(return_data.size()));
            std::mutex block_lock;
            std::atomic<long int> jobs(0);

            std::map<sha256_2, Block*> block_tree;
            std::map<sha256_2, Block*> prev_tree;
            {
                ThreadPool TP;
                uint64_t position = 0;
                while (position + 8 < return_data.size()) {
                    uint64_t block_size = *(reinterpret_cast<uint64_t*>(&return_data[position]));
                    position += 8;

                    if (position + block_size <= return_data.size()) {
                        jobs++;

                        char* block_buff = &return_data[position];

                        parse_block_async(TP, jobs, block_buff, block_size, block_lock, block_tree, prev_tree, false);

                    } else {
                        DEBUG_COUT("position + block_size <= return_data.size()");
                        DEBUG_COUT(std::to_string(position) + "+" + std::to_string(block_size) + "<=" + std::to_string(return_data.size()));
                        break;
                    }

                    position += block_size;
                }

                while (jobs.load(std::memory_order_acquire) != 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

            DEBUG_COUT("PARSE BLOCKCHAIN COMPLETE");
            //            for (auto block_pair : block_tree) {
            //                block_queue.enqueue(block_pair.second);
            //            }

            apply_block_chain(block_tree, prev_tree, core_block.first, true);

            DEBUG_COUT("READ BLOCK COMPLETE");
        }
    }
}

void ControllerImplementation::start_main_loop()
{
    std::thread(&ControllerImplementation::main_loop, this).detach();
}

std::string ControllerImplementation::add_pack_to_queue(std::string_view pack, std::string_view url)
{
    if (url == RPC_PING) {
        parse_S_PING(pack);
    } else if (url == RPC_TX) {
        parse_B_TX(pack);
    } else if (url == RPC_PRETEND_BLOCK) {
        parse_C_PRETEND_BLOCK(pack);
    } else if (url == RPC_APPROVE) {
        parse_C_APPROVE(pack);
    } else if (url == RPC_DISAPPROVE) {
        parse_C_DISAPPROVE(pack);
    } else if (url == RPC_APPROVE_BLOCK) {
        parse_C_APPROVE_BLOCK(pack);
    } else if (url == RPC_LAST_BLOCK) {
        return parse_S_LAST_BLOCK(pack);
    } else if (url == RPC_GET_BLOCK) {
        return parse_S_GET_BLOCK(pack);
    } else if (url == RPC_GET_CHAIN) {
        return parse_S_GET_CHAIN(pack);
    } else if (url == RPC_GET_CORE_LIST) {
        return parse_S_GET_CORE_LIST(pack);
    } else if (url == RPC_GET_CORE_ADDR) {
        return parse_S_GET_CORE_ADDR(pack);
    }

    return "";
}

std::string ControllerImplementation::get_str_address()
{
    return Addres;
}

std::string ControllerImplementation::get_last_block_str()
{
    return bin2hex(last_applyed_block);
}

std::atomic<std::map<std::string, std::pair<int, int>>*>& ControllerImplementation::get_wallet_statistics()
{

    return BC->get_wallet_statistics();
}

std::atomic<std::deque<std::pair<std::string, uint64_t>>*>& ControllerImplementation::get_wallet_request_addreses()
{
    return BC->get_wallet_request_addreses();
}

void ControllerImplementation::main_loop()
{
    while (goon) {

        const uint64_t get_arr_size = 128;
        bool need_actualize = true;

        bool no_sleep = false;

        if (transactions.size() < 200) {
            static TX* tx_arr[get_arr_size];
            uint64_t got_tx = tx_queue.try_dequeue_bulk(tx_arr, get_arr_size);

            if (got_tx) {
                no_sleep = true;
                std::copy(&tx_arr[0], &tx_arr[got_tx], std::back_inserter(transactions));
            }
        }

        {
            static Block* block_arr[get_arr_size];
            uint64_t got_block = block_queue.try_dequeue_bulk(block_arr, get_arr_size);

            if (got_block) {
                no_sleep = true;

                for (uint i = 0; i < got_block; i++) {
                    Block* block = block_arr[i];

                    if (blocks.find(block->get_prev_hash()) == blocks.end()) {
                        need_actualize = true;
                    }

                    if (blocks.insert({ block->get_block_hash(), block }).second) {
                        ;
                    } else {
                        delete block;
                    }
                }
            }

            for (std::pair<sha256_2, Block*> block_pair : blocks) {
                if (block_approve[block_pair.first].find(Addres) == block_approve[block_pair.first].end()
                    && block_disapprove[block_pair.first].find(Addres) == block_disapprove[block_pair.first].end()) {

                    Block* block = block_pair.second;
                    if (block->get_prev_hash() == last_applyed_block) {
                        if (BC->can_apply_block(block)) {
                            approve_block(block);
                        } else {
                            disapprove_block(block);
                        }
                    }
                }
            }
        }

        {
            static ApproveRecord* approve_arr[get_arr_size];
            uint64_t got_approve = approve_queue.try_dequeue_bulk(approve_arr, get_arr_size);

            if (got_approve) {
                no_sleep = true;

                for (uint i = 0; i < got_approve; i++) {
                    apply_approve(approve_arr[i]);
                }
            }
        }

        if (master && try_make_block()) {
            no_sleep = true;
        }

        {
            uint64_t timestamp = static_cast<uint64_t>(std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()).time_since_epoch().count());

            if (timestamp - last_sync_timestamp > 60) {
                DEBUG_COUT("sync_core_lists");
                std::thread(&CoreController::sync_core_lists, &cores).detach();

                last_sync_timestamp = timestamp;
            }

            if (prev_timestamp > last_actualization_timestamp) {
                last_actualization_timestamp = prev_timestamp;
            }

            if (timestamp - last_actualization_timestamp > 1) {
                need_actualize = true;
            }

            if (need_actualize) {
                no_sleep = true;
                actualize_chain();
                need_actualize = false;
                last_actualization_timestamp = timestamp;
            }
        }

        if (no_sleep) {
            ;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void ControllerImplementation::parse_S_PING(std::string_view)
{
}

void ControllerImplementation::parse_B_TX(std::string_view pack)
{
    uint64_t index = 0;

    uint64_t tx_size;
    std::string_view tx_size_arr(&pack[index], pack.size() - index);
    uint64_t varint_size = read_varint(tx_size, tx_size_arr);
    if (varint_size < 1) {
        DEBUG_COUT("corrupt varint size");
        return;
    }
    index += varint_size;

    while (tx_size > 0) {
        if (index + tx_size >= pack.size()) {
            DEBUG_COUT("corrupt tx size");
            return;
        }
        std::string_view tx_sw(&pack[index], tx_size);
        index += tx_size;

        TX* p_tx = new TX;
        if (p_tx->parse(tx_sw)) {
            tx_queue.enqueue(p_tx);
        } else {
            delete p_tx;
            DEBUG_COUT("corrupt tx");
        }

        std::string_view tx_size_arr(&pack[index], pack.size() - index);
        uint64_t varint_size = read_varint(tx_size, tx_size_arr);
        if (varint_size < 1) {
            DEBUG_COUT("corrupt varint size");
            return;
        }
        index += varint_size;
    }
}

void ControllerImplementation::parse_C_APPROVE_BLOCK(std::string_view pack)
{
    uint64_t block_size;
    uint64_t approve_size;

    uint64_t offset = read_varint(approve_size, pack);
    if (!offset) {
        DEBUG_COUT("corrupt pack");
    }

    std::string_view approve_sw(pack.data() + offset, approve_size);
    auto* p_ar = new ApproveRecord;
    if (p_ar->parse(approve_sw)) {
        p_ar->approve = true;
        approve_queue.enqueue(p_ar);
    } else {
        DEBUG_COUT("corrupt pack");
        delete p_ar;
    }

    std::string_view block_raw(pack.data() + offset + approve_size, pack.size() - (offset + approve_size));

    offset = read_varint(block_size, block_raw);
    if (!offset) {
        DEBUG_COUT("corrupt pack");
    }
    std::string_view block_sw(block_raw.data() + offset, block_size);
    Block* block = parse_block(block_sw);

    if (block) {
        block_queue.enqueue(block);
    } else {
        DEBUG_COUT("corrupt pack");
        delete block;
    }
}

void ControllerImplementation::parse_C_PRETEND_BLOCK(std::string_view pack)
{
    std::string_view block_sw(pack);
    Block* block = parse_block(block_sw);

    if (block) {
        block_queue.enqueue(block);
    }
}

void ControllerImplementation::parse_C_APPROVE(std::string_view pack)
{
    std::string_view approve_sw(pack);
    auto* p_ar = new ApproveRecord;
    if (p_ar->parse(approve_sw)) {
        p_ar->approve = true;
        approve_queue.enqueue(p_ar);
    } else {
        delete p_ar;
    }
}

void ControllerImplementation::parse_C_DISAPPROVE(std::string_view pack)
{
    std::string_view approve_sw(pack);
    auto* p_ar = new ApproveRecord;
    if (p_ar->parse(approve_sw)) {
        p_ar->approve = false;
        approve_queue.enqueue(p_ar);
    } else {
        delete p_ar;
    }
}

std::string ControllerImplementation::parse_S_LAST_BLOCK(std::string_view)
{
    std::string last_block;
    last_block.insert(last_block.end(), last_applyed_block.begin(), last_applyed_block.end());
    char* p_timestamp = reinterpret_cast<char*>(&prev_timestamp);
    last_block.insert(last_block.end(), p_timestamp, p_timestamp + 8);
    return last_block;
}

std::string ControllerImplementation::parse_S_GET_BLOCK(std::string_view pack)
{

    if (pack.size() < 32) {
        return "";
    }

    sha256_2 block_hash;
    std::copy_n(pack.begin(), 32, block_hash.begin());

    if (blocks.find(block_hash) != blocks.end()) {
        std::string return_string;
        {
            auto& block_data = blocks[block_hash]->get_data();

            uint64_t block_size = block_data.size();
            return_string.insert(return_string.end(), reinterpret_cast<char*>(&block_size), reinterpret_cast<char*>(&block_size) + sizeof(uint64_t));
            return_string.insert(return_string.end(), block_data.begin(), block_data.end());
        }
        return return_string;
    }
    return "";
}

std::string ControllerImplementation::parse_S_GET_CHAIN(std::string_view pack)
{
    sha256_2 prev_block = { { 0 } };
    DEBUG_COUT(std::to_string(pack.size()));

    if (pack.size() < 32) {
        return "";
    }

    std::copy_n(pack.begin(), 32, prev_block.begin());

    std::string chain;
    sha256_2 got_block = last_applyed_block;

    DEBUG_COUT(bin2hex(prev_block));
    DEBUG_COUT(bin2hex(got_block));

    while (got_block != prev_block && blocks.find(got_block) != blocks.end()) {
        auto& block_data = blocks[got_block]->get_data();

        uint64_t block_size = block_data.size();
        chain.insert(chain.end(), reinterpret_cast<char*>(&block_size), reinterpret_cast<char*>(&block_size) + sizeof(uint64_t));
        chain.insert(chain.end(), block_data.begin(), block_data.end());

        got_block = blocks[got_block]->get_prev_hash();
    }

    return chain;
}

std::string ControllerImplementation::parse_S_GET_CORE_LIST(std::string_view)
{
    return cores.get_core_list();
}

std::string ControllerImplementation::parse_S_GET_CORE_ADDR(std::string_view pack)
{
    std::string addr_req_str(pack);
    std::stringstream ss(addr_req_str);
    std::string item;
    std::vector<std::string> elems;
    while (std::getline(ss, item, ':')) {
        elems.push_back(std::move(item));
    }

    if (elems.size() == 2) {
        cores.add_core(elems[0], std::stoi(elems[1]));
    }

    return Addres;
}

void ControllerImplementation::approve_block(Block* p_block)
{
    auto* p_ar = new ApproveRecord;
    p_ar->make(p_block->get_block_hash(), PrivKey, PubKey);
    p_ar->approve = true;

    distribute(p_ar);
    apply_approve(p_ar);
}

void ControllerImplementation::disapprove_block(Block* p_block)
{
    auto* p_ar = new ApproveRecord;
    p_ar->make(p_block->get_block_hash(), PrivKey, PubKey);
    p_ar->approve = false;

    distribute(p_ar);
    apply_approve(p_ar);
}

void ControllerImplementation::apply_approve(ApproveRecord* p_ar)
{
    sha256_2 block_hash;
    std::copy(p_ar->block_hash.begin(), p_ar->block_hash.end(), block_hash.begin());

    auto bin_addres = get_address(p_ar->pub_key);
    std::string addr = "0x" + bin2hex(bin_addres);

    if (p_ar->approve) {
        if (!block_approve[block_hash].insert({ addr, p_ar }).second) {
            DEBUG_COUT("APPROVE ALREADY PRESENT NOT CREATED");
            delete p_ar;
        }

        auto approve_size = block_approve[block_hash].size();
        auto m_approve = min_approve();
        DEBUG_COUT(std::to_string(approve_size) + "\t" + std::to_string(m_approve));
        if (approve_size >= m_approve) {

            if (blocks.find(block_hash) != blocks.end()) {
                Block* block = blocks[block_hash];
                if (block->get_prev_hash() == last_applyed_block) {
                    if (BC->can_apply_block(block)) {
                        if (block->get_block_type() == BLOCK_TYPE_STATE) {
                            make_clean();
                        }

                        BC->apply_block(block);
                        write_block(block);
                    } else {
                        DEBUG_COUT("!BC->can_apply_block(block)");
                    }
                } else {
                    DEBUG_COUT("block->get_prev_hash != last_applyed_block");
                }
            } else {
                DEBUG_COUT("blocks.find(block_hash) == blocks.end()");
            }
        }
    } else {
        if (!block_disapprove[block_hash].insert({ addr, p_ar }).second) {
            delete p_ar;
        }
    }
}

void ControllerImplementation::distribute(Block* block)
{
    std::string send_pack;
    send_pack.insert(send_pack.end(), block->get_data().begin(), block->get_data().end());

    cores.send_no_return(RPC_PRETEND_BLOCK, send_pack);
}

void ControllerImplementation::distribute(ApproveRecord* p_ar)
{
    std::string approve_str = p_ar->approve ? RPC_APPROVE : RPC_DISAPPROVE;
    std::string send_pack;
    send_pack.insert(send_pack.end(), p_ar->data.begin(), p_ar->data.end());

    cores.send_no_return(approve_str, send_pack);
}

uint64_t ControllerImplementation::min_approve()
{
    uint64_t min_approve = 0;

    sha256_2 curr_block = last_applyed_block;
    uint i = 0;

    while (blocks.find(curr_block) != blocks.end() && i < 5) {
        i++;

        uint64_t curr_block_approve = block_approve[curr_block].size() + block_disapprove[curr_block].size();

        if (min_approve) {
            if (curr_block_approve < min_approve) {
                min_approve = curr_block_approve;
            }
        } else {
            min_approve = curr_block_approve;
        }

        curr_block = blocks[curr_block]->get_prev_hash();
    }

    min_approve = min_approve * 51 / 100;

    if (min_approve) {
        return min_approve;
    }
    return 1;
}

void ControllerImplementation::write_block(Block* block)
{
    prev_timestamp = block->get_block_timestamp();
    last_applyed_block = block->get_block_hash();

    prev_day = prev_timestamp / DAY_IN_SECONDS;
    prev_state = block->get_block_type();

    auto theTime = static_cast<time_t>(prev_timestamp);
    struct tm* aTime = localtime(&theTime);

    int day = aTime->tm_mday;
    int month = aTime->tm_mon + 1; // Month is 0 - 11, add 1 to get a jan-dec 1-12 concept
    int year = aTime->tm_year + 1900; // Year is # years since 1900

    //                          y   m   d   t
    const uint file_name_size = 38; //4 + 2 + 2 + 4 + 1;
    char file_name[file_name_size] = { 0 };
    std::snprintf(file_name, file_name_size, "%04d%02d%02d.blk", year, month, day);

    std::string file_path = path + "/" + file_name;

    DEBUG_COUT("file_path\t" + file_path);

    uint64_t block_size = block->get_data().size() /* + approve_buff.size()*/;

    std::ofstream myfile;
    myfile.open(file_path.c_str(), std::ios::out | std::ios::app | std::ios::binary);
    myfile.write(reinterpret_cast<char*>(&block_size), sizeof(uint64_t));
    myfile.write(block->get_data().data(), static_cast<int64_t>(block->get_data().size()));
    myfile.close();

    {
        uint64_t timestamp = static_cast<uint64_t>(std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()).time_since_epoch().count());
        auto common_block = dynamic_cast<CommonBlock*>(block);
        DEBUG_COUT("block size and latency\t" + std::to_string(common_block->get_txs().size()) + "\t" + std::to_string(timestamp - prev_timestamp));
    }
}

bool ControllerImplementation::try_make_block()
{
    if (last_applyed_block == last_created_block) {
        uint64_t timestamp = static_cast<uint64_t>(std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()).time_since_epoch().count());

        if (prev_timestamp >= timestamp) {
            return false;
        }

        Block* block = nullptr;
        uint64_t block_state = BLOCK_TYPE_COMMON;

        uint64_t current_day = (timestamp + 1) / DAY_IN_SECONDS;

        if (prev_state == BLOCK_TYPE_FORGING) {
            block_state = BLOCK_TYPE_STATE;
            DEBUG_COUT("BLOCK_STATE_STATE");
        } else {
            if (current_day > prev_day && prev_state != BLOCK_TYPE_FORGING) {
                block_state = BLOCK_TYPE_FORGING;
                DEBUG_COUT("BLOCK_STATE_FORGING");
            } else {
                block_state = BLOCK_TYPE_COMMON;
            }
        }

        switch (block_state) {
        case BLOCK_TYPE_COMMON:
            if (timestamp - statistics_timestamp > 600) {
                block = BC->make_statistics_block(timestamp);
                statistics_timestamp = timestamp;
            } else {
                if (!transactions.empty()) {
                    block = BC->make_common_block(timestamp, transactions);
                }
            }
            break;
        case BLOCK_TYPE_FORGING:
            DEBUG_COUT("BLOCK_TYPE_FORGING");
            block = BC->make_forging_block(timestamp);
            break;
        case BLOCK_TYPE_STATE:
            DEBUG_COUT("BLOCK_TYPE_STATE");
            block = BC->make_state_block(timestamp);
            break;
        }

        if (block) {
            if (BC->can_apply_block(block)) {
                last_created_block = block->get_block_hash();
                blocks.insert({ block->get_block_hash(), block });
                distribute(block);
                approve_block(block);
                return true;
            }

            /// FUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUU
            DEBUG_COUT("FUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUU");
            std::this_thread::sleep_for(std::chrono::seconds(3));
            exit(1);
        }
    }
    return false;
}

void ControllerImplementation::make_clean()
{
    ;
}
