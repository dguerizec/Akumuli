#include <iostream>

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE Main

#include <boost/test/unit_test.hpp>

#include <apr.h>

#include "akumuli.h"
#include "storage_engine/blockstore.h"
#include "storage_engine/volume.h"
#include "storage_engine/nbtree.h"
#include "log_iface.h"

void test_logger(aku_LogLevel tag, const char* msg) {
    AKU_UNUSED(tag);
    BOOST_MESSAGE(msg);
}

struct AkumuliInitializer {
    AkumuliInitializer() {
        apr_initialize();
        Akumuli::Logger::set_logger(&test_logger);
    }
};

static AkumuliInitializer initializer;

using namespace Akumuli;
using namespace Akumuli::StorageEngine;


enum class ScanDir {
    FWD, BWD
};

void test_nbtree_roots_collection(u32 N, u32 begin, u32 end) {
    ScanDir dir = begin < end ? ScanDir::FWD : ScanDir::BWD;
    std::shared_ptr<BlockStore> bstore = BlockStoreBuilder::create_memstore();
    std::vector<LogicAddr> addrlist;  // should be empty at first
    auto collection = std::make_shared<NBTreeExtentsList>(42, addrlist, bstore);
    for (u32 i = 0; i < N; i++) {
        collection->append(i, i);
    }

    // Read data back
    std::unique_ptr<NBTreeIterator> it = collection->search(begin, end);

    aku_Status status;
    size_t sz;
    size_t outsz = dir == ScanDir::FWD ? end - begin : begin - end;
    std::vector<aku_Timestamp> ts(outsz, 0xF0F0F0F0);
    std::vector<double> xs(outsz, -1);
    std::tie(status, sz) = it->read(ts.data(), xs.data(), outsz);

    BOOST_REQUIRE_EQUAL(sz, outsz);

    BOOST_REQUIRE_EQUAL(status, AKU_SUCCESS);

    if (dir == ScanDir::FWD) {
        for (u32 i = 0; i < outsz; i++) {
            const auto curr = i + begin;
            if (ts[i] != curr) {
                BOOST_FAIL("Invalid timestamp at " << i << ", expected: " << curr << ", actual: " << ts[i]);
            }
            if (!same_value(xs[i], curr)) {
                BOOST_FAIL("Invalid value at " << i << ", expected: " << curr << ", actual: " << xs[i]);
            }
        }
    } else {
        for (u32 i = 0; i < outsz; i++) {
            const auto curr = begin - i;
            if (ts[i] != curr) {
                BOOST_FAIL("Invalid timestamp at " << i << ", expected: " << curr << ", actual: " << ts[i]);
            }
            if (!same_value(xs[i], curr)) {
                BOOST_FAIL("Invalid value at " << i << ", expected: " << curr << ", actual: " << xs[i]);
            }
        }

    }
}

BOOST_AUTO_TEST_CASE(Test_nbtree_rc_append_1) {
    test_nbtree_roots_collection(100, 0, 100);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_rc_append_2) {
    test_nbtree_roots_collection(2000, 0, 2000);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_rc_append_3) {
    test_nbtree_roots_collection(200000, 0, 200000);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_rc_append_4) {
    test_nbtree_roots_collection(100, 99, 0);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_rc_append_5) {
    test_nbtree_roots_collection(2000, 1999, 0);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_rc_append_6) {
    test_nbtree_roots_collection(200000, 199999, 0);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_rc_append_rand_read) {
    for (int i = 0; i < 100; i++) {
        auto N = static_cast<u32>(rand()) % 200000u;
        auto from = static_cast<u32>(rand()) % N;
        auto to = static_cast<u32>(rand()) % N;
        test_nbtree_roots_collection(N, from, to);
    }
}

void test_nbtree_chunked_read(u32 N, u32 begin, u32 end, u32 chunk_size) {
    ScanDir dir = begin < end ? ScanDir::FWD : ScanDir::BWD;
    std::shared_ptr<BlockStore> bstore = BlockStoreBuilder::create_memstore();
    std::vector<LogicAddr> addrlist;  // should be empty at first
    auto collection = std::make_shared<NBTreeExtentsList>(42, addrlist, bstore);

    for (u32 i = 0; i < N; i++) {
        collection->append(i, i);
    }

    // Read data back
    std::unique_ptr<NBTreeIterator> it = collection->search(begin, end);

    aku_Status status;
    size_t sz;
    std::vector<aku_Timestamp> ts(chunk_size, 0xF0F0F0F0);
    std::vector<double> xs(chunk_size, -1);

    u32 total_size = 0u;
    aku_Timestamp ts_seen = begin;
    while(true) {
        std::tie(status, sz) = it->read(ts.data(), xs.data(), chunk_size);

        if (sz == 0 && status == AKU_SUCCESS) {
            BOOST_FAIL("Invalid iterator output, sz=0, status=" << status);
        }
        total_size += sz;

        BOOST_REQUIRE(status == AKU_SUCCESS || status == AKU_ENO_DATA);

        if (dir == ScanDir::FWD) {
            for (u32 i = 0; i < sz; i++) {
                const auto curr = ts_seen;
                if (ts[i] != curr) {
                    BOOST_FAIL("Invalid timestamp at " << i << ", expected: " << curr << ", actual: " << ts[i]);
                }
                if (!same_value(xs[i], curr)) {
                    BOOST_FAIL("Invalid value at " << i << ", expected: " << curr << ", actual: " << xs[i]);
                }
                ts_seen = ts[i] + 1;
            }
        } else {
            for (u32 i = 0; i < sz; i++) {
                const auto curr = ts_seen;
                if (ts[i] != curr) {
                    BOOST_FAIL("Invalid timestamp at " << i << ", expected: " << curr << ", actual: " << ts[i]);
                }
                if (!same_value(xs[i], curr)) {
                    BOOST_FAIL("Invalid value at " << i << ", expected: " << curr << ", actual: " << xs[i]);
                }
                ts_seen = ts[i] - 1;
            }
        }

        if (status == AKU_ENO_DATA || ts_seen == end) {
            break;
        }
    }
    if (ts_seen != end) {
        BOOST_FAIL("Bad range, expected: " << end << ", actual: " << ts_seen <<
                   " dir: " << (dir == ScanDir::FWD ? "forward" : "backward"));
    }
    size_t outsz = dir == ScanDir::FWD ? end - begin : begin - end;
    BOOST_REQUIRE_EQUAL(total_size, outsz);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_chunked_read) {
    for (u32 i = 0; i < 100; i++) {
        auto N = static_cast<u32>(rand() % 200000);
        auto from = static_cast<u32>(rand()) % N;
        auto to = static_cast<u32>(rand()) % N;
        auto chunk = static_cast<u32>(rand()) % N;
        test_nbtree_chunked_read(N, from, to, chunk);
    }
}

void check_tree_consistency(std::shared_ptr<BlockStore> bstore, size_t level, NBTreeExtent const* extent) {
    NBTreeExtent::check_extent(extent, bstore, level);
}

void test_reopen_storage(i32 Npages, i32 Nitems) {
    LogicAddr last_one = EMPTY_ADDR;
    std::shared_ptr<BlockStore> bstore =
        BlockStoreBuilder::create_memstore([&last_one](LogicAddr addr) { last_one = addr; });
    std::vector<LogicAddr> addrlist;  // should be empty at first
    auto collection = std::make_shared<NBTreeExtentsList>(42, addrlist, bstore);

    u32 nleafs = 0;
    u32 nitems = 0;
    for (u32 i = 0; true; i++) {
        if (collection->append(i, i)) {
            // addrlist changed
            auto newroots = collection->get_roots();
            if (newroots == addrlist) {
                BOOST_FAIL("Roots collection must change");
            }
            std::swap(newroots, addrlist);
            nleafs++;
            if (static_cast<i32>(nleafs) == Npages) {
                nitems = i;
                break;
            }
        }
        if (static_cast<i32>(i) == Nitems) {
            nitems = i;
            break;
        }
    }

    addrlist = collection->close();

    BOOST_REQUIRE_EQUAL(addrlist.back(), last_one);

    // TODO: check attempt to open tree using wrong id!
    collection = std::make_shared<NBTreeExtentsList>(42, addrlist, bstore);

    collection->force_init();

    auto extents = collection->get_extents();
    for (size_t i = 0; i < extents.size(); i++) {
        auto extent = extents[i];
        check_tree_consistency(bstore, i, extent);
    }

    std::unique_ptr<NBTreeIterator> it = collection->search(0, nitems);
    std::vector<aku_Timestamp> ts(nitems, 0);
    std::vector<double> xs(nitems, 0);
    aku_Status status = AKU_SUCCESS;
    size_t sz = 0;
    std::tie(status, sz) = it->read(ts.data(), xs.data(), nitems);
    BOOST_REQUIRE(sz == nitems);
    BOOST_REQUIRE(status == AKU_SUCCESS);
    for (u32 i = 0; i < nitems; i++) {
        if (ts[i] != i) {
            BOOST_FAIL("Invalid timestamp at " << i);
        }
        if (!same_value(xs[i], static_cast<double>(i))) {
            BOOST_FAIL("Invalid timestamp at " << i);
        }
    }
}

BOOST_AUTO_TEST_CASE(Test_nbtree_reopen_1) {
    test_reopen_storage(-1, 1);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_reopen_2) {
    test_reopen_storage(1, -1);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_reopen_3) {
    test_reopen_storage(2, -1);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_reopen_4) {
    test_reopen_storage(32, -1);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_reopen_5) {
    test_reopen_storage(33, -1);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_reopen_6) {
    test_reopen_storage(32*32, -1);
}

//! Reopen storage that has been closed without final commit.
void test_storage_recovery_status(u32 N, u32 N_values) {
    LogicAddr last_block = EMPTY_ADDR;
    auto cb = [&last_block] (LogicAddr addr) {
        last_block = addr;
    };
    std::shared_ptr<BlockStore> bstore = BlockStoreBuilder::create_memstore(cb);
    std::vector<LogicAddr> addrlist;  // should be empty at first
    auto collection = std::make_shared<NBTreeExtentsList>(42, addrlist, bstore);

    u32 nleafs = 0;
    u32 nitems = 0;
    for (u32 i = 0; true; i++) {
        if (collection->append(i, i)) {
            // addrlist changed
            auto newroots = collection->get_roots();
            if (newroots == addrlist) {
                BOOST_FAIL("Roots collection must change");
            }
            std::swap(newroots, addrlist);
            auto status = NBTreeExtentsList::repair_status(addrlist);
            BOOST_REQUIRE(status == NBTreeExtentsList::RepairStatus::REPAIR);
            nleafs++;
            if (nleafs == N) {
                nitems = i;
                break;
            }
        }
        if (i == N_values) {
            nitems = i;
            break;
        }
    }
    addrlist = collection->close();
    auto status = NBTreeExtentsList::repair_status(addrlist);
    BOOST_REQUIRE(status == NBTreeExtentsList::RepairStatus::OK);
    BOOST_REQUIRE(addrlist.back() == last_block);
    AKU_UNUSED(nitems);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_recovery_status_1) {
    test_storage_recovery_status(~0u, 32);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_recovery_status_2) {
    test_storage_recovery_status(2, ~0u);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_recovery_status_3) {
    test_storage_recovery_status(32, ~0u);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_recovery_status_4) {
    test_storage_recovery_status(32*32, ~0u);
}

//! Reopen storage that has been closed without final commit.
void test_storage_recovery(u32 N_blocks, u32 N_values) {
    LogicAddr last_block = EMPTY_ADDR;
    auto cb = [&last_block] (LogicAddr addr) {
        last_block = addr;
    };
    std::shared_ptr<BlockStore> bstore = BlockStoreBuilder::create_memstore(cb);
    std::vector<LogicAddr> addrlist;  // should be empty at first
    auto collection = std::make_shared<NBTreeExtentsList>(42, addrlist, bstore);

    u32 nleafs = 0;
    u32 nitems = 0;
    for (u32 i = 0; true; i++) {
        if (collection->append(i, i)) {
            // addrlist changed
            auto newroots = collection->get_roots();
            if (newroots == addrlist) {
                BOOST_FAIL("Roots collection must change");
            }
            std::swap(newroots, addrlist);
            auto status = NBTreeExtentsList::repair_status(addrlist);
            BOOST_REQUIRE(status == NBTreeExtentsList::RepairStatus::REPAIR);
            nleafs++;
            if (nleafs == N_blocks) {
                nitems = i;
                break;
            }
        }
        if (i == N_values) {
            nitems = i;
            break;
        }
    }

    addrlist = collection->get_roots();

    //for (auto addr: addrlist) {
    //    std::cout << "Dbg print for " << addr << std::endl;
    //    NBTreeExtentsList::debug_print(addr, bstore);
    //}

    // delete roots collection
    collection.reset();

    // TODO: check attempt to open tree using wrong id!
    collection = std::make_shared<NBTreeExtentsList>(42, addrlist, bstore);

    collection->force_init();

    auto extents = collection->get_extents();
    for (size_t i = 0; i < extents.size(); i++) {
        auto extent = extents[i];
        check_tree_consistency(bstore, i, extent);
    }

    std::unique_ptr<NBTreeIterator> it = collection->search(0, nitems);
    std::vector<aku_Timestamp> ts(nitems, 0);
    std::vector<double> xs(nitems, 0);
    aku_Status status = AKU_SUCCESS;
    size_t sz = 0;
    std::tie(status, sz) = it->read(ts.data(), xs.data(), nitems);
    if (addrlist.empty()) {
        // Expect zero, data was stored in single leaf-node.
        BOOST_REQUIRE(sz == 0);
    } else {
        // `sz` value can't be equal to N because some data should be lost!
        BOOST_REQUIRE(sz < nitems);
    }
    // Note: `status` should be equal to AKU_SUCCESS if size of the destination
    // is equal to array's length. Otherwise iterator should return AKU_ENO_DATA
    // as an indication that all data-elements have ben read.
    BOOST_REQUIRE(status == AKU_ENO_DATA || status  == AKU_SUCCESS);
    for (u32 i = 0; i < sz; i++) {
        if (ts[i] != i) {
            BOOST_FAIL("Invalid timestamp at " << i);
        }
        if (!same_value(xs[i], static_cast<double>(i))) {
            BOOST_FAIL("Invalid timestamp at " << i);
        }
    }
}

BOOST_AUTO_TEST_CASE(Test_nbtree_recovery_1) {
    test_storage_recovery(~0u, 10);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_recovery_2) {
    test_storage_recovery(1, ~0u);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_recovery_3) {
    test_storage_recovery(31, ~0u);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_recovery_4) {
    test_storage_recovery(32, ~0u);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_recovery_5) {
    test_storage_recovery(33, ~0u);
}

BOOST_AUTO_TEST_CASE(Test_nbtree_recovery_6) {
    test_storage_recovery(33*33, ~0u);
}
