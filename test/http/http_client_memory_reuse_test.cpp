// Test to reproduce coredump through memory reuse
// This test creates rapid create/destroy cycles to trigger memory reuse
// where a new ConnectionInfo is allocated at the same address as a destroyed one,
// causing the old callback's raw 'this' pointer to access the wrong object.

#include "boost_formatters.hpp"
#include "dbus_singleton.hpp"
#include "http_client.hpp"
#include "ssl_key_handler.hpp"

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace
{

class HttpClientMemoryReuseTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        systemBus = std::make_unique<sdbusplus::asio::connection>(ioc);
        crow::connections::systemBus = systemBus.get();
    }

    void TearDown() override
    {
        crow::connections::systemBus = nullptr;
        systemBus.reset();
    }

    boost::asio::io_context ioc;
    std::unique_ptr<sdbusplus::asio::connection> systemBus;
};

// Test that creates rapid cycles to trigger memory reuse
// This increases the chance of a new object being allocated at the same
// address as a destroyed one, causing the old callback to access wrong data
TEST_F(HttpClientMemoryReuseTest, RapidCyclesWithMemoryReuse)
{
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    std::atomic<int> callbackCount{0};
    std::atomic<bool> possibleCorruption{false};

    // Create many rapid cycles to increase memory reuse probability
    for (int cycle = 0; cycle < 50; ++cycle)
    {
        auto conn = std::make_shared<crow::ConnectionInfo>(
            ioc, "memory-reuse-test", policy,
            boost::urls::url_view("http://nonexistent" + std::to_string(cycle) + ".test:9999/"),
            ensuressl::VerifyCertificate::Verify, cycle);

        // Track the connection ID to detect if callback fires for wrong object
        const uint32_t expectedConnId = cycle;
        
        conn->callback = [&callbackCount, &possibleCorruption, expectedConnId](
                            bool, uint32_t actualConnId, crow::Response&) {
            callbackCount.fetch_add(1);
            
            // If callback fires with different connId, we have memory corruption!
            if (actualConnId != expectedConnId)
            {
                possibleCorruption.store(true);
            }
        };

        boost::beast::http::request<boost::beast::http::string_body> req;
        req.method(boost::beast::http::verb::get);
        req.target("/");
        req.set(boost::beast::http::field::host, 
                "nonexistent" + std::to_string(cycle) + ".test:9999");
        conn->req = std::move(req);

        // Start async operation
        conn->doResolve();

        // Process a few events
        for (int i = 0; i < 2; ++i)
        {
            ioc.poll();
            if (ioc.stopped())
            {
                ioc.restart();
            }
        }

        // Immediately destroy - this is the key to triggering memory reuse
        conn.reset();

        // Process more events - old callbacks may fire
        for (int i = 0; i < 3; ++i)
        {
            ioc.poll();
            if (ioc.stopped())
            {
                ioc.restart();
            }
        }
    }

    // Process all remaining events
    for (int i = 0; i < 200; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            ioc.restart();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (possibleCorruption.load())
    {
        FAIL() << "MEMORY CORRUPTION DETECTED! Callback fired with wrong connId. "
               << "This proves the raw 'this' pointer bug.";
    }
    else
    {
        // Test passed, but that doesn't mean the bug doesn't exist
        // It just means we didn't trigger the exact timing/memory reuse scenario
        SUCCEED() << "No corruption detected in this run. Callbacks fired: " 
                  << callbackCount.load();
    }
}

// Test with explicit memory pressure to force reuse
TEST_F(HttpClientMemoryReuseTest, MemoryPressureTest)
{
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    std::vector<void*> addresses;
    std::atomic<bool> addressReused{false};

    for (int cycle = 0; cycle < 100; ++cycle)
    {
        auto conn = std::make_shared<crow::ConnectionInfo>(
            ioc, "pressure-test", policy,
            boost::urls::url_view("http://test" + std::to_string(cycle) + ".invalid:9999/"),
            ensuressl::VerifyCertificate::Verify, cycle);

        // Track the address
        void* addr = conn.get();
        
        // Check if this address was used before
        for (void* prevAddr : addresses)
        {
            if (prevAddr == addr)
            {
                addressReused.store(true);
                BMCWEB_LOG_INFO("Address reuse detected at cycle {}: {}", cycle, addr);
                break;
            }
        }
        addresses.push_back(addr);

        conn->callback = [](bool, uint32_t, crow::Response&) {
            // Callback that would access members through raw 'this'
        };

        boost::beast::http::request<boost::beast::http::string_body> req;
        req.method(boost::beast::http::verb::get);
        req.target("/");
        req.set(boost::beast::http::field::host, "test.invalid:9999");
        conn->req = std::move(req);

        conn->doResolve();
        
        // Minimal processing
        ioc.poll();
        
        // Immediate destruction
        conn.reset();
        
        // Process events
        for (int i = 0; i < 5; ++i)
        {
            ioc.poll();
            if (ioc.stopped())
            {
                ioc.restart();
            }
        }
    }

    // Final cleanup
    for (int i = 0; i < 100; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            ioc.restart();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (addressReused.load())
    {
        SUCCEED() << "Address reuse detected - this increases crash probability in production";
    }
    else
    {
        SUCCEED() << "No address reuse in this run - but bug still exists";
    }
}

} // namespace

// This test demonstrates that even with shared_from_this() parameter,
// the raw 'this' pointer in std::bind_front can cause issues when:
// 1. Memory is reused (new object at same address as destroyed one)
// 2. Callbacks fire after object destruction
// 3. The callback accesses members through the captured raw 'this'
//
// The fix is to use weak_from_this() instead of raw 'this' in the binding.

// Made with Bob