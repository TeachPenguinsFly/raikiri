#include <raikiri/books/file_store.hpp>
#include <raikiri/books/routes.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace raikiri::books {
namespace {

class TempFile {
public:
    TempFile()
        : path_{(std::filesystem::temp_directory_path() /
                 ("raikiri-books-" + std::to_string(::getpid()) + "-" +
                  ::testing::UnitTest::GetInstance()->current_test_info()->name()) /
                 "books.jsonl")
                    .string()} {
        std::filesystem::remove_all(std::filesystem::path{path_}.parent_path());
    }

    ~TempFile() { std::filesystem::remove_all(std::filesystem::path{path_}.parent_path()); }

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
};

const auto k_wall = [] { return UnixNanos{1'000}; };

TEST(BookNames, EachNameMeansItsKind) {
    EXPECT_EQ(reserved_kind("manual"), BookKind::Manual);
    EXPECT_EQ(reserved_kind("manual:vova"), BookKind::Manual);
    EXPECT_EQ(reserved_kind("client:acme"), BookKind::Client);
    EXPECT_EQ(reserved_kind("hedge"), BookKind::Hedge);
    EXPECT_EQ(reserved_kind("joe"), BookKind::Unknown);
    EXPECT_EQ(reserved_kind("client:"), BookKind::Unknown);
    EXPECT_EQ(reserved_kind("client:a b"), BookKind::Unknown);
    EXPECT_EQ(reserved_kind("manual:a&b=c"), BookKind::Unknown);
}

TEST(BookNames, ALoginMapsToItsBookAndNoLoginToTheSharedOne) {
    EXPECT_EQ(manual_book_name("vova"), "manual:vova");
    EXPECT_EQ(manual_book_name("a.b-c_d@e"), "manual:a.b-c_d@e");
    EXPECT_EQ(manual_book_name(""), "manual");
    // a header value that could break out of the query string admind forwards
    EXPECT_EQ(manual_book_name("x&default_book=hedge"), "manual");
    EXPECT_EQ(manual_book_name(std::string(65, 'a')), "manual");
}

TEST(BookIds, AllocationStartsAboveWhatAPersonPicksAndNeverReachesTheSignBit) {
    EXPECT_EQ(next_id({}).value(), k_first_allocated);
    const std::vector<Book> low{{.id = 2, .kind = BookKind::Strategy, .name = "a"}};
    EXPECT_EQ(next_id(low).value(), k_first_allocated);
    const std::vector<Book> high{{.id = 2000, .kind = BookKind::Strategy, .name = "a"}};
    EXPECT_EQ(next_id(high).value(), 2001);
    const std::vector<Book> parked{{.id = 40000, .kind = BookKind::Strategy, .name = "a"}};
    EXPECT_EQ(next_id(parked).value(), k_first_allocated);
    const std::vector<Book> full{{.id = k_last_allocated, .kind = BookKind::Client, .name = "client:z"}};
    EXPECT_EQ(next_id(full).error(), BookError::Exhausted);
}

TEST(BookRegistry, OpensEachNameOnceWithItsOwnId) {
    MemoryBookStore store;
    Registry books{store, k_wall};
    ASSERT_TRUE(books.load().has_value());
    EXPECT_EQ(books.kind_of(k_manual_strategy_id), BookKind::Manual);

    const auto vova = books.open("manual:vova").value();
    const auto acme = books.open("client:acme").value();
    const auto hedge = books.open("hedge").value();
    EXPECT_EQ(vova.id, k_first_allocated);
    EXPECT_EQ(acme.id, k_first_allocated + 1);
    EXPECT_EQ(hedge.id, k_first_allocated + 2);
    EXPECT_EQ(books.open("manual:vova").value().id, vova.id);
    EXPECT_EQ(books.find(acme.id)->name, "client:acme");
    EXPECT_EQ(books.open("joe").error(), BookError::BadName);
}

TEST(BookRegistry, ResolvingMakesAnOperatorsAndTheHedgeBookButNeverAClients) {
    MemoryBookStore store;
    Registry books{store, k_wall};
    ASSERT_TRUE(books.load().has_value());
    EXPECT_EQ(books.resolve("hedge").value().kind, BookKind::Hedge);
    EXPECT_EQ(books.resolve("manual:vova").value().kind, BookKind::Manual);
    EXPECT_EQ(books.resolve("client:acme").error(), BookError::NotFound);
    ASSERT_TRUE(books.open("client:acme").has_value());
    EXPECT_EQ(books.resolve("client:acme").value().kind, BookKind::Client);
    EXPECT_EQ(books.resolve("nonsense").error(), BookError::NotFound);
}

TEST(BookRegistry, AStrategyKeepsItsIdAndAnotherNameCannotTakeIt) {
    MemoryBookStore store;
    Registry books{store, k_wall};
    ASSERT_TRUE(books.load().has_value());
    EXPECT_EQ(books.register_strategy(7, "mm").value().kind, BookKind::Strategy);
    EXPECT_EQ(books.register_strategy(7, "mm").value().id, 7);
    EXPECT_EQ(books.register_strategy(7, "arb").error(), BookError::IdTaken);
    EXPECT_EQ(books.register_strategy(k_manual_strategy_id, "impostor").error(), BookError::IdTaken);
    EXPECT_EQ(books.register_strategy(8, "hedge").error(), BookError::BadName);
    EXPECT_EQ(books.register_strategy(8, "client:acme").error(), BookError::BadName);
    // the id a client got is not a strategy's to claim
    const auto acme = books.open("client:acme").value();
    EXPECT_EQ(books.register_strategy(acme.id, "mm2").error(), BookError::IdTaken);
}

TEST(BookRegistry, OnlyManualAndClientBooksTakeIntents) {
    MemoryBookStore store;
    Registry books{store, k_wall};
    ASSERT_TRUE(books.load().has_value());
    const auto vova = books.open("manual:vova").value();
    const auto acme = books.open("client:acme").value();
    const auto hedge = books.open("hedge").value();
    ASSERT_TRUE(books.register_strategy(7, "mm").has_value());
    EXPECT_TRUE(books.takes_intents(ClientOrderId::make(k_manual_strategy_id, 1)));
    EXPECT_TRUE(books.takes_intents(ClientOrderId::make(vova.id, 1)));
    EXPECT_TRUE(books.takes_intents(ClientOrderId::make(acme.id, 1)));
    EXPECT_FALSE(books.takes_intents(ClientOrderId::make(hedge.id, 1)));
    EXPECT_FALSE(books.takes_intents(ClientOrderId::make(7, 1)));
    EXPECT_FALSE(books.takes_intents(ClientOrderId::make(4000, 1)));
    EXPECT_FALSE(books.takes_intents(ClientOrderId::make_child(vova.id, 1)));
}

TEST(BookFileStore, IdsAndNamesSurviveARestartAndAreNotHandedOutAgain) {
    const TempFile file;
    std::uint16_t vova = 0;
    std::uint16_t acme = 0;
    {
        FileBookStore store{file.path()};
        Registry books{store, k_wall};
        ASSERT_TRUE(books.load().has_value());
        vova = books.open("manual:vova").value().id;
        acme = books.open("client:acme").value().id;
        ASSERT_TRUE(books.register_strategy(7, "mm").has_value());
    }
    FileBookStore store{file.path()};
    Registry books{store, k_wall};
    ASSERT_TRUE(books.load().has_value());
    EXPECT_EQ(books.find("manual:vova")->id, vova);
    EXPECT_EQ(books.find(acme)->name, "client:acme");
    EXPECT_EQ(books.find(acme)->created.ns, 1'000U);
    EXPECT_EQ(books.list().size(), 4U); // no second `manual`
    EXPECT_EQ(books.find(7)->kind, BookKind::Strategy);
    EXPECT_GT(books.open("hedge").value().id, acme);
}

TEST(BookFileStore, TwoProcessesOnOneFileSeeEachOthersBooks) {
    const TempFile file;
    FileBookStore one_store{file.path()};
    FileBookStore other_store{file.path()};
    Registry one{one_store, k_wall};
    Registry other{other_store, k_wall};
    ASSERT_TRUE(one.load().has_value());
    ASSERT_TRUE(other.load().has_value());

    const auto vova = one.open("manual:vova").value();
    EXPECT_EQ(other.find(vova.id), nullptr);
    // the intent ring is the first other sees of it
    EXPECT_TRUE(other.takes_intents(ClientOrderId::make(vova.id, 1)));
    EXPECT_EQ(other.find("manual:vova")->id, vova.id);

    // a stale view must not allocate the id it has not seen
    const auto acme = one.open("client:acme").value();
    const auto hedge = other.open("hedge").value();
    EXPECT_NE(hedge.id, acme.id);
    // and both opening one name land on one book
    EXPECT_EQ(one.open("hedge").value().id, hedge.id);
}

TEST(BookFileStore, ACutShortLastLineNeverHappenedAndAnyOtherBadLineRefusesTheFile) {
    const TempFile file;
    {
        FileBookStore store{file.path()};
        Registry books{store, k_wall};
        ASSERT_TRUE(books.load().has_value());
        ASSERT_TRUE(books.open("manual:vova").has_value());
    }
    {
        std::ofstream out{file.path(), std::ios::app};
        out << R"({"id":1025,"kind":"client","name":"client:ha)";
    }
    {
        FileBookStore store{file.path()};
        Registry books{store, k_wall};
        ASSERT_TRUE(books.load().has_value());
        EXPECT_EQ(books.find("client:ha"), nullptr);
        // the next append does not join the torn line
        const auto acme = books.open("client:acme").value();
        EXPECT_EQ(acme.id, 1025);
    }
    {
        FileBookStore store{file.path()};
        Registry books{store, k_wall};
        ASSERT_TRUE(books.load().has_value());
        EXPECT_EQ(books.find("client:acme")->id, 1025);
    }
    {
        std::ofstream out{file.path(), std::ios::app};
        out << "not json\n";
    }
    FileBookStore store{file.path()};
    Registry books{store, k_wall};
    EXPECT_EQ(books.load().error(), BookError::Store);
    EXPECT_EQ(books.open("client:new").error(), BookError::Store);
}

TEST(BookRoutes, AClientEdgeOpensAClientBookAndNothingElse) {
    MemoryBookStore store;
    Registry books{store, k_wall};
    ASSERT_TRUE(books.load().has_value());
    const BookRoutes routes{books};
    EXPECT_FALSE(routes({.method = "GET", .path = "/v1/other"}).has_value());

    const auto made = routes({.method = "POST", .path = "/v1/books", .body = R"({"name":"client:acme"})"});
    ASSERT_TRUE(made.has_value());
    EXPECT_EQ(made->status, 201);
    EXPECT_NE(made->body.find(R"("name":"client:acme")"), std::string::npos);
    EXPECT_NE(made->body.find(R"("kind":"client")"), std::string::npos);
    EXPECT_EQ(routes({.method = "POST", .path = "/v1/books", .body = R"({"name":"client:acme"})"})->status, 200);

    EXPECT_EQ(routes({.method = "POST", .path = "/v1/books", .body = R"({"name":"hedge"})"})->status, 422);
    EXPECT_EQ(routes({.method = "POST", .path = "/v1/books", .body = R"({"name":"manual:x"})"})->status, 422);
    EXPECT_EQ(routes({.method = "POST", .path = "/v1/books", .body = "{"})->status, 400);
    EXPECT_EQ(routes({.method = "DELETE", .path = "/v1/books"})->status, 405);

    const auto list = routes({.method = "GET", .path = "/v1/books"});
    EXPECT_EQ(list->status, 200);
    EXPECT_NE(list->body.find(R"("name":"manual")"), std::string::npos);
    EXPECT_NE(list->body.find("client:acme"), std::string::npos);
}

} // namespace
} // namespace raikiri::books
