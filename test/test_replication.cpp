/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include "testsettings.hpp"
#ifdef TEST_REPLICATION

#include <algorithm>
#include <memory>

#include <realm.hpp>
#include <realm/util/features.h>
#include <realm/util/file.hpp>
#include <realm/replication.hpp>

#include "test.hpp"
#include "test_table_helper.hpp"

using namespace realm;
using namespace realm::util;
using namespace realm::test_util;
using unit_test::TestContext;


// Test independence and thread-safety
// -----------------------------------
//
// All tests must be thread safe and independent of each other. This
// is required because it allows for both shuffling of the execution
// order and for parallelized testing.
//
// In particular, avoid using std::rand() since it is not guaranteed
// to be thread safe. Instead use the API offered in
// `test/util/random.hpp`.
//
// All files created in tests must use the TEST_PATH macro (or one of
// its friends) to obtain a suitable file system path. See
// `test/util/test_path.hpp`.
//
//
// Debugging and the ONLY() macro
// ------------------------------
//
// A simple way of disabling all tests except one called `Foo`, is to
// replace TEST(Foo) with ONLY(Foo) and then recompile and rerun the
// test suite. Note that you can also use filtering by setting the
// environment varible `UNITTEST_FILTER`. See `README.md` for more on
// this.
//
// Another way to debug a particular test, is to copy that test into
// `experiments/testcase.cpp` and then run `sh build.sh
// check-testcase` (or one of its friends) from the command line.


namespace {

class MyTrivialReplication : public TrivialReplication {
public:
    MyTrivialReplication(const std::string& path)
        : TrivialReplication(path)
    {
    }

    void replay_transacts(SharedGroup& target, util::Logger& replay_logger)
    {
        for (const Buffer<char>& changeset : m_changesets)
            apply_changeset(changeset.data(), changeset.size(), target, &replay_logger);
        m_changesets.clear();
    }

    void initiate_session(version_type) override
    {
        // No-op
    }

    void terminate_session() noexcept override
    {
        // No-op
    }

    HistoryType get_history_type() const noexcept override
    {
        return hist_None;
    }

    int get_history_schema_version() const noexcept override
    {
        return 0;
    }

    bool is_upgradable_history_schema(int) const noexcept override
    {
        REALM_ASSERT(false);
        return false;
    }

    void upgrade_history_schema(int) override
    {
        REALM_ASSERT(false);
    }

    _impl::History* get_history() override
    {
        return nullptr;
    }

private:
    version_type prepare_changeset(const char* data, size_t size, version_type orig_version) override
    {
        m_incoming_changeset = Buffer<char>(size); // Throws
        std::copy(data, data + size, m_incoming_changeset.data());
        // Make space for the new changeset in m_changesets such that we can be
        // sure no exception will be thrown whan adding the changeset in
        // finalize_changeset().
        m_changesets.reserve(m_changesets.size() + 1); // Throws
        return orig_version + 1;
    }

    void finalize_changeset() noexcept override
    {
        // The following operation will not throw due to the space reservation
        // carried out in prepare_new_changeset().
        m_changesets.push_back(std::move(m_incoming_changeset));
    }

    Buffer<char> m_incoming_changeset;
    std::vector<Buffer<char>> m_changesets;
};

namespace {

void my_table_add_columns(TableRef t)
{
    t->add_column(type_Int, "my_int");
    t->add_column(type_Bool, "my_bool");
    t->add_column(type_Float, "my_float");
    t->add_column(type_Double, "my_double");
    t->add_column(type_String, "my_string");
    t->add_column(type_Binary, "my_binary");
    t->add_column(type_OldDateTime, "my_olddatetime");
    DescriptorRef sub_descr1;
    t->add_column(type_Table, "my_subtable", &sub_descr1);
    t->add_column(type_Mixed, "my_mixed");

    sub_descr1->add_column(type_Int, "a");
    DescriptorRef sub_descr2;
    sub_descr1->add_column(type_Table, "b", &sub_descr2);
    sub_descr1->add_column(type_Int, "c");

    sub_descr2->add_column(type_Int, "first");
}
}


TEST(Replication_General)
{
    SHARED_GROUP_TEST_PATH(path_1);
    SHARED_GROUP_TEST_PATH(path_2);

    CHECK(Version::has_feature(Feature::feature_Replication));

    MyTrivialReplication repl(path_1);
    SharedGroup sg_1(repl);
    {
        WriteTransaction wt(sg_1);
        TableRef table = wt.add_table("my_table");
        my_table_add_columns(table);
        table->add_empty_row();
        wt.commit();
    }
    {
        WriteTransaction wt(sg_1);
        TableRef table = wt.get_table("my_table");
        char buf[] = {'1'};
        BinaryData bin(buf);
        Mixed mix;
        mix.set_int(1);
        set(table, 0, 2, true, 2.0f, 2.0, "xx", bin, 728, nullptr, mix);
        add(table, 3, true, 3.0f, 3.0, "xxx", bin, 729, nullptr, mix);
        insert(table, 0, 1, true, 1.0f, 1.0, "x", bin, 727, nullptr, mix);

        add(table, 3, true, 3.0f, 0.0, "", bin, 729, nullptr, mix); // empty string
        add(table, 3, true, 3.0f, 1.0, "", bin, 729, nullptr, mix); // empty string
        wt.commit();
    }
    {
        WriteTransaction wt(sg_1);
        TableRef table = wt.get_table("my_table");
        table->set_int(0, 0, 9);
        wt.commit();
    }
    {
        WriteTransaction wt(sg_1);
        TableRef table = wt.get_table("my_table");
        table->set_int(0, 0, 10);
        wt.commit();
    }
    // Test Table::move_last_over()
    {
        WriteTransaction wt(sg_1);
        TableRef table = wt.get_table("my_table");
        char buf[] = {'9'};
        BinaryData bin(buf);
        Mixed mix;
        mix.set_float(9.0f);
        insert(table, 2, 8, false, 8.0f, 8.0, "y8", bin, 282, nullptr, mix);
        insert(table, 1, 9, false, 9.0f, 9.0, "y9", bin, 292, nullptr, mix);
        wt.commit();
    }
    {
        WriteTransaction wt(sg_1);
        TableRef table = wt.get_table("my_table");
        table->move_last_over(1);
        wt.commit();
    }

    util::Logger& replay_logger = test_context.logger;
    SharedGroup sg_2(path_2);
    repl.replay_transacts(sg_2, replay_logger);

    {
        ReadTransaction rt_1(sg_1);
        ReadTransaction rt_2(sg_2);
        rt_1.get_group().verify();
        rt_2.get_group().verify();
        CHECK(rt_1.get_group() == rt_2.get_group());
        auto table = rt_2.get_table("my_table");
        CHECK_EQUAL(6, table->size());
        CHECK_EQUAL(10, table->get_int(0, 0));
        CHECK_EQUAL(3, table->get_int(0, 1));
        CHECK_EQUAL(2, table->get_int(0, 2));
        CHECK_EQUAL(8, table->get_int(0, 3));

        StringData sd1 = table->get_string(4, 4);

        CHECK(!sd1.is_null());
    }
}


void check(TestContext& test_context, SharedGroup& sg_1, const ReadTransaction& rt_2)
{
    ReadTransaction rt_1(sg_1);
    rt_1.get_group().verify();
    rt_2.get_group().verify();
    CHECK(rt_1.get_group() == rt_2.get_group());
}


TEST(Replication_Timestamp)
{
    SHARED_GROUP_TEST_PATH(path_1);
    SHARED_GROUP_TEST_PATH(path_2);

    MyTrivialReplication repl(path_1);
    SharedGroup sg_1(repl);
    {
        WriteTransaction wt(sg_1);
        TableRef table = wt.add_table("t");

        // Add nullable Timestamp column
        table->add_column(type_Timestamp, "ts", true);

        wt.commit();
    }
    {
        WriteTransaction wt(sg_1);
        TableRef table = wt.get_table("t");

        // First row is to have a row that we can test move_last_over() on later
        table->add_empty_row();
        CHECK(table->get_timestamp(0, 0).is_null());

        table->add_empty_row();
        table->set_timestamp(0, 1, Timestamp(5, 6));
        table->add_empty_row();
        table->set_timestamp(0, 2, Timestamp(1, 2));
        wt.commit();
    }
    {
        WriteTransaction wt(sg_1);
        TableRef table = wt.get_table("t");

        // Overwrite non-null with null to test that
        // TransactLogParser::parse_one(InstructionHandler& handler) correctly will see a set_null instruction
        // and not a set_new_date instruction
        table->set_timestamp(0, 1, Timestamp{});

        // Overwrite non-null with other non-null
        table->set_timestamp(0, 2, Timestamp(3, 4));
        wt.commit();
    }
    {
        // move_last_over
        WriteTransaction wt(sg_1);
        TableRef table = wt.get_table("t");
        table->move_last_over(0);
        wt.commit();
    }

    util::Logger& replay_logger = test_context.logger;
    SharedGroup sg_2(path_2);
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt_1(sg_1);
        rt_1.get_group().verify();
        ConstTableRef table = rt_1.get_table("t");
        CHECK_EQUAL(2, table->size());
        CHECK(table->get_timestamp(0, 0) == Timestamp(3, 4));
        CHECK(table->get_timestamp(0, 1).is_null());
    }
}


TEST(Replication_Links)
{
    // This test checks that all the links-related stuff works through
    // replication. It does that in a chained manner where the output of one
    // test acts as the input of the next one. This is to save boilerplate code,
    // and to make the test scenarios slightly more varied and realistic.
    //
    // The following operations are covered (for cyclic stuff, see
    // Replication_LinkCycles):
    //
    // - add_empty_row to origin table
    // - add_empty_row to target table
    // - insert link + link list
    // - change link
    // - nullify link
    // - insert link into list
    // - remove link from list
    // - move link inside list
    // - clear link list
    // - move_last_over on origin table
    // - move_last_over on target table
    // - clear origin table
    // - clear target table
    // - insert and remove non-link-type columns in origin table
    // - Insert and remove link-type columns in origin table
    // - Insert and remove columns in target table

    SHARED_GROUP_TEST_PATH(path_1);
    SHARED_GROUP_TEST_PATH(path_2);

    util::Logger& replay_logger = test_context.logger;

    MyTrivialReplication repl(path_1);
    SharedGroup sg_1(repl);
    SharedGroup sg_2(path_2);

    // First create two origin tables and two target tables, and add some links
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1 = wt.add_table("origin_1");
        TableRef origin_2 = wt.add_table("origin_2");
        TableRef target_1 = wt.add_table("target_1");
        TableRef target_2 = wt.add_table("target_2");
        target_1->add_column(type_Int, "t_1");
        target_2->add_column(type_Int, "t_2");
        target_1->add_empty_row(2);
        target_2->add_empty_row(2);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1 = wt.get_table("origin_1");
        TableRef origin_2 = wt.get_table("origin_2");
        TableRef target_1 = wt.get_table("target_1");
        origin_1->add_column_link(type_LinkList, "o_1_ll_1", *target_1);
        origin_2->add_column(type_Int, "o_2_f_1");
        origin_2->add_empty_row(2);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1: LL_1->T_1
    // O_2: F_1
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1 = wt.get_table("origin_1");
        TableRef origin_2 = wt.get_table("origin_2");
        TableRef target_1 = wt.get_table("target_1");
        origin_1->insert_column(0, type_Int, "o_1_f_2");
        origin_2->insert_column_link(0, type_Link, "o_2_l_2", *target_1);
        origin_2->set_link(0, 0, 1); // O_2_L_2[0] -> T_1[1]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1: F_2   LL_1->T_1
    // O_2: L_2->T_1   F_1
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1 = wt.get_table("origin_1");
        TableRef origin_2 = wt.get_table("origin_2");
        TableRef target_1 = wt.get_table("target_1");
        TableRef target_2 = wt.get_table("target_2");
        origin_1->insert_column_link(0, type_Link, "o_1_l_3", *target_1);
        origin_2->add_column_link(type_LinkList, "o_2_ll_3", *target_2);
        origin_2->get_linklist(2, 0)->add(1); // O_2_LL_3[0] -> T_2[1]
        origin_2->get_linklist(2, 1)->add(0); // O_2_LL_3[1] -> T_2[0]
        origin_2->get_linklist(2, 1)->add(1); // O_2_LL_3[1] -> T_2[1]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1: L_3->T_1   F_2   LL_1->T_1
    // O_2: L_2->T_1   F_1   LL_3->T_2
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1 = wt.get_table("origin_1");
        TableRef origin_2 = wt.get_table("origin_2");
        TableRef target_2 = wt.get_table("target_2");
        origin_1->insert_column_link(2, type_Link, "o_1_l_4", *target_2);
        origin_2->add_column_link(type_Link, "o_2_l_4", *target_2);
        origin_2->set_link(3, 0, 1); // O_2_L_4[0] -> T_2[1]
        origin_2->set_link(3, 1, 0); // O_2_L_4[1] -> T_2[0]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1: L_3->T_1   F_2   L_4->T_2   LL_1->T_1
    // O_2: L_2->T_1   F_1   LL_3->T_2   L_4->T_2
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1 = wt.get_table("origin_1");
        TableRef origin_2 = wt.get_table("origin_2");
        TableRef target_1 = wt.get_table("target_1");
        TableRef target_2 = wt.get_table("target_2");
        origin_1->insert_column(3, type_Int, "o_1_f_5");
        origin_2->insert_column(3, type_Int, "o_2_f_5");
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1: L_3->T_1   F_2   L_4->T_2   F_5   LL_1->T_1
    // O_2: L_2->T_1   F_1   LL_3->T_2   F_5   L_4->T_2
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1 = wt.get_table("origin_1");
        origin_1->add_empty_row(2);
        origin_1->set_link(0, 1, 0);          // O_1_L_3[1] -> T_1[0]
        origin_1->set_link(2, 0, 0);          // O_1_L_4[0] -> T_2[0]
        origin_1->set_link(2, 1, 1);          // O_1_L_4[1] -> T_2[1]
        origin_1->get_linklist(4, 1)->add(0); // O_1_LL_1[1] -> T_1[0]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // null       T_2[0]     []                     T_1[1]     [ T_2[1] ]             T_2[1]
    // T_1[0]     T_2[1]     [ T_1[0] ]             null       [ T_2[0], T_2[1] ]     T_2[0]
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK(origin_1->is_attached());
        CHECK(origin_2->is_attached());
        CHECK(target_1->is_attached());
        CHECK(target_2->is_attached());
        CHECK_EQUAL(2, origin_1->size());
        CHECK_EQUAL(2, origin_2->size());
        CHECK_EQUAL(2, target_1->size());
        CHECK_EQUAL(2, target_2->size());
        CHECK_EQUAL(5, origin_1->get_column_count());
        CHECK_EQUAL(5, origin_2->get_column_count());
        CHECK_EQUAL(1, target_1->get_column_count());
        CHECK_EQUAL(1, target_2->get_column_count());
        CHECK_EQUAL(type_Link, origin_1->get_column_type(0));
        CHECK_EQUAL(type_Int, origin_1->get_column_type(1));
        CHECK_EQUAL(type_Link, origin_1->get_column_type(2));
        CHECK_EQUAL(type_Int, origin_1->get_column_type(3));
        CHECK_EQUAL(type_LinkList, origin_1->get_column_type(4));
        CHECK_EQUAL(type_Link, origin_2->get_column_type(0));
        CHECK_EQUAL(type_Int, origin_2->get_column_type(1));
        CHECK_EQUAL(type_LinkList, origin_2->get_column_type(2));
        CHECK_EQUAL(type_Int, origin_2->get_column_type(3));
        CHECK_EQUAL(type_Link, origin_2->get_column_type(4));
        CHECK_EQUAL(target_1, origin_1->get_link_target(0));
        CHECK_EQUAL(target_2, origin_1->get_link_target(2));
        CHECK_EQUAL(target_1, origin_1->get_link_target(4));
        CHECK_EQUAL(target_1, origin_2->get_link_target(0));
        CHECK_EQUAL(target_2, origin_2->get_link_target(2));
        CHECK_EQUAL(target_2, origin_2->get_link_target(4));
        CHECK(origin_1->is_null_link(0, 0));
        CHECK_EQUAL(0, origin_1->get_link(0, 1));
        CHECK_EQUAL(0, origin_1->get_link(2, 0));
        CHECK_EQUAL(1, origin_1->get_link(2, 1));
        CHECK_EQUAL(0, origin_1->get_linklist(4, 0)->size());
        CHECK_EQUAL(1, origin_1->get_linklist(4, 1)->size());
        CHECK_EQUAL(0, origin_1->get_linklist(4, 1)->get(0).get_index());
        CHECK_EQUAL(1, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->size());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->get(0).get_index());
        CHECK_EQUAL(2, origin_2->get_linklist(2, 1)->size());
        CHECK_EQUAL(0, origin_2->get_linklist(2, 1)->get(0).get_index());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 1)->get(1).get_index());
        CHECK_EQUAL(1, origin_2->get_link(4, 0));
        CHECK_EQUAL(0, origin_2->get_link(4, 1));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
    }

    // Check that an empty row can be added to an origin table
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        origin_1_w->add_empty_row();
        origin_1_w->set_int(1, 2, 13);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // null       T_2[0]     []                     T_1[1]     [ T_2[1] ]             T_2[1]
    // T_1[0]     T_2[1]     [ T_1[0] ]             null       [ T_2[0], T_2[1] ]     T_2[0]
    // null       null       []
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(3, origin_1->size());
        CHECK_EQUAL(13, origin_1->get_int(1, 2));
        CHECK(origin_1->is_null_link(0, 0));
        CHECK_EQUAL(0, origin_1->get_link(0, 1));
        CHECK(origin_1->is_null_link(0, 2));
        CHECK_EQUAL(0, origin_1->get_link(2, 0));
        CHECK_EQUAL(1, origin_1->get_link(2, 1));
        CHECK(origin_1->is_null_link(2, 2));
        CHECK_EQUAL(0, origin_1->get_linklist(4, 0)->size());
        CHECK_EQUAL(1, origin_1->get_linklist(4, 1)->size());
        CHECK_EQUAL(0, origin_1->get_linklist(4, 1)->get(0).get_index());
        CHECK_EQUAL(0, origin_1->get_linklist(4, 2)->size());
        CHECK_EQUAL(1, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->size());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->get(0).get_index());
        CHECK_EQUAL(2, origin_2->get_linklist(2, 1)->size());
        CHECK_EQUAL(0, origin_2->get_linklist(2, 1)->get(0).get_index());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 1)->get(1).get_index());
        CHECK_EQUAL(1, origin_2->get_link(4, 0));
        CHECK_EQUAL(0, origin_2->get_link(4, 1));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
    }

    // Check that an empty row can be added to a target table
    {
        WriteTransaction wt(sg_1);
        TableRef target_1_w = wt.get_table("target_1");
        target_1_w->add_empty_row();
        target_1_w->set_int(0, 2, 17);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // null       T_2[0]     []                     T_1[1]     [ T_2[1] ]             T_2[1]
    // T_1[0]     T_2[1]     [ T_1[0] ]             null       [ T_2[0], T_2[1] ]     T_2[0]
    // null       null       []
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(3, target_1->size());
        CHECK_EQUAL(17, target_1->get_int(0, 2));
        CHECK(origin_1->is_null_link(0, 0));
        CHECK_EQUAL(0, origin_1->get_link(0, 1));
        CHECK(origin_1->is_null_link(0, 2));
        CHECK_EQUAL(0, origin_1->get_link(2, 0));
        CHECK_EQUAL(1, origin_1->get_link(2, 1));
        CHECK(origin_1->is_null_link(2, 2));
        CHECK_EQUAL(0, origin_1->get_linklist(4, 0)->size());
        CHECK_EQUAL(1, origin_1->get_linklist(4, 1)->size());
        CHECK_EQUAL(0, origin_1->get_linklist(4, 1)->get(0).get_index());
        CHECK_EQUAL(0, origin_1->get_linklist(4, 2)->size());
        CHECK_EQUAL(1, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->size());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->get(0).get_index());
        CHECK_EQUAL(2, origin_2->get_linklist(2, 1)->size());
        CHECK_EQUAL(0, origin_2->get_linklist(2, 1)->get(0).get_index());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 1)->get(1).get_index());
        CHECK_EQUAL(1, origin_2->get_link(4, 0));
        CHECK_EQUAL(0, origin_2->get_link(4, 1));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
    }

    // Check that a non-empty row can be added to an origin table
    {
        WriteTransaction wt(sg_1);
        TableRef origin_2_w = wt.get_table("origin_2");
        origin_2_w->insert_empty_row(2);
        origin_2_w->set_link(0, 2, 1); // O_2_L_2[2] -> T_1[1]
        origin_2_w->set_int(1, 2, 19);
        // linklist is empty by default
        origin_2_w->set_int(3, 2, 0);
        origin_2_w->set_link(4, 2, 0); // O_2_L_4[2] -> T_2[0]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // null       T_2[0]     []                     T_1[1]     [ T_2[1] ]             T_2[1]
    // T_1[0]     T_2[1]     [ T_1[0] ]             null       [ T_2[0], T_2[1] ]     T_2[0]
    // null       null       []                     T_1[1]     []                     T_2[0]
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(3, origin_2->size());
        CHECK_EQUAL(19, origin_2->get_int(1, 2));
        CHECK(origin_1->is_null_link(0, 0));
        CHECK_EQUAL(0, origin_1->get_link(0, 1));
        CHECK(origin_1->is_null_link(0, 2));
        CHECK_EQUAL(0, origin_1->get_link(2, 0));
        CHECK_EQUAL(1, origin_1->get_link(2, 1));
        CHECK(origin_1->is_null_link(2, 2));
        CHECK_EQUAL(0, origin_1->get_linklist(4, 0)->size());
        CHECK_EQUAL(1, origin_1->get_linklist(4, 1)->size());
        CHECK_EQUAL(0, origin_1->get_linklist(4, 1)->get(0).get_index());
        CHECK_EQUAL(0, origin_1->get_linklist(4, 2)->size());
        CHECK_EQUAL(1, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_2->get_link(0, 2));
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->size());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->get(0).get_index());
        CHECK_EQUAL(2, origin_2->get_linklist(2, 1)->size());
        CHECK_EQUAL(0, origin_2->get_linklist(2, 1)->get(0).get_index());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 1)->get(1).get_index());
        CHECK_EQUAL(0, origin_2->get_linklist(2, 2)->size());
        CHECK_EQUAL(1, origin_2->get_link(4, 0));
        CHECK_EQUAL(0, origin_2->get_link(4, 1));
        CHECK_EQUAL(0, origin_2->get_link(4, 2));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(2, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
    }

    // Check that a link can be changed
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        origin_1_w->set_link(0, 2, 1);  // null -> non-null
        origin_2_w->nullify_link(0, 2); // non-null -> null
        origin_2_w->set_link(4, 2, 1);  // non-null -> non-null
        // Removes O_2_L_2[2] -> T_1[1]  and  O_2_L_4[2] -> T_2[0]
        // Adds    O_1_L_3[2] -> T_1[1]  and  O_2_L_4[2] -> T_2[1]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // null       T_2[0]     []                     T_1[1]     [ T_2[1] ]             T_2[1]
    // T_1[0]     T_2[1]     [ T_1[0] ]             null       [ T_2[0], T_2[1] ]     T_2[0]
    // T_1[1]     null       []                     null       []                     T_2[1]
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK(origin_1->is_null_link(0, 0));
        CHECK_EQUAL(0, origin_1->get_link(0, 1));
        CHECK_EQUAL(1, origin_1->get_link(0, 2));
        CHECK_EQUAL(0, origin_1->get_link(2, 0));
        CHECK_EQUAL(1, origin_1->get_link(2, 1));
        CHECK(origin_1->is_null_link(2, 2));
        CHECK_EQUAL(0, origin_1->get_linklist(4, 0)->size());
        CHECK_EQUAL(1, origin_1->get_linklist(4, 1)->size());
        CHECK_EQUAL(0, origin_1->get_linklist(4, 1)->get(0).get_index());
        CHECK_EQUAL(0, origin_1->get_linklist(4, 2)->size());
        CHECK_EQUAL(1, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK(origin_2->is_null_link(0, 2));
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->size());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->get(0).get_index());
        CHECK_EQUAL(2, origin_2->get_linklist(2, 1)->size());
        CHECK_EQUAL(0, origin_2->get_linklist(2, 1)->get(0).get_index());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 1)->get(1).get_index());
        CHECK_EQUAL(0, origin_2->get_linklist(2, 2)->size());
        CHECK_EQUAL(1, origin_2->get_link(4, 0));
        CHECK_EQUAL(0, origin_2->get_link(4, 1));
        CHECK_EQUAL(1, origin_2->get_link(4, 2));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 4));
    }

    // Check that a link can be added to an empty link list
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        LinkViewRef link_list_1_2_w = origin_1_w->get_linklist(4, 2);
        LinkViewRef link_list_2_2_w = origin_2_w->get_linklist(2, 2);
        link_list_1_2_w->add(0); // O_1_LL_1[2] -> T_1[0]
        link_list_1_2_w->add(1); // O_1_LL_1[2] -> T_1[1]
        link_list_2_2_w->add(0); // O_2_LL_3[2] -> T_2[0]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // null       T_2[0]     []                     T_1[1]     [ T_2[1] ]             T_2[1]
    // T_1[0]     T_2[1]     [ T_1[0] ]             null       [ T_2[0], T_2[1] ]     T_2[0]
    // T_1[1]     null       [ T_1[0], T_1[1] ]     null       [ T_2[0] ]             T_2[1]
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK(origin_1->is_null_link(0, 0));
        CHECK_EQUAL(0, origin_1->get_link(0, 1));
        CHECK_EQUAL(1, origin_1->get_link(0, 2));
        CHECK_EQUAL(0, origin_1->get_link(2, 0));
        CHECK_EQUAL(1, origin_1->get_link(2, 1));
        CHECK(origin_1->is_null_link(2, 2));
        CHECK_EQUAL(0, origin_1->get_linklist(4, 0)->size());
        CHECK_EQUAL(1, origin_1->get_linklist(4, 1)->size());
        CHECK_EQUAL(0, origin_1->get_linklist(4, 1)->get(0).get_index());
        CHECK(link_list_1_2->is_attached());
        CHECK_EQUAL(link_list_1_2, origin_1->get_linklist(4, 2));
        CHECK_EQUAL(2, link_list_1_2->size());
        CHECK_EQUAL(0, link_list_1_2->get(0).get_index());
        CHECK_EQUAL(1, link_list_1_2->get(1).get_index());
        CHECK_EQUAL(1, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK(origin_2->is_null_link(0, 2));
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->size());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->get(0).get_index());
        CHECK_EQUAL(2, origin_2->get_linklist(2, 1)->size());
        CHECK_EQUAL(0, origin_2->get_linklist(2, 1)->get(0).get_index());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 1)->get(1).get_index());
        CHECK(link_list_2_2->is_attached());
        CHECK_EQUAL(link_list_2_2, origin_2->get_linklist(2, 2));
        CHECK_EQUAL(1, link_list_2_2->size());
        CHECK_EQUAL(0, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(1, origin_2->get_link(4, 0));
        CHECK_EQUAL(0, origin_2->get_link(4, 1));
        CHECK_EQUAL(1, origin_2->get_link(4, 2));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(2, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 4));
    }

    // Check that a link can be removed from a link list, and that a link can be
    // added to a non-empty link list
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        LinkViewRef link_list_1_2_w = origin_1_w->get_linklist(4, 2);
        LinkViewRef link_list_2_2_w = origin_2_w->get_linklist(2, 2);
        link_list_1_2_w->remove(0); // Remove  O_1_LL_1[2] -> T_1[0]
        link_list_2_2_w->add(1);    // Add     O_2_LL_3[2] -> T_2[1]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // null       T_2[0]     []                     T_1[1]     [ T_2[1] ]             T_2[1]
    // T_1[0]     T_2[1]     [ T_1[0] ]             null       [ T_2[0], T_2[1] ]     T_2[0]
    // T_1[1]     null       [ T_1[1] ]             null       [ T_2[0], T_2[1] ]     T_2[1]
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK(origin_1->is_null_link(0, 0));
        CHECK_EQUAL(0, origin_1->get_link(0, 1));
        CHECK_EQUAL(1, origin_1->get_link(0, 2));
        CHECK_EQUAL(0, origin_1->get_link(2, 0));
        CHECK_EQUAL(1, origin_1->get_link(2, 1));
        CHECK(origin_1->is_null_link(2, 2));
        CHECK_EQUAL(0, origin_1->get_linklist(4, 0)->size());
        CHECK_EQUAL(1, origin_1->get_linklist(4, 1)->size());
        CHECK_EQUAL(0, origin_1->get_linklist(4, 1)->get(0).get_index());
        CHECK(link_list_1_2->is_attached());
        CHECK_EQUAL(link_list_1_2, origin_1->get_linklist(4, 2));
        CHECK_EQUAL(1, link_list_1_2->size());
        CHECK_EQUAL(1, link_list_1_2->get(0).get_index());
        CHECK_EQUAL(1, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK(origin_2->is_null_link(0, 2));
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->size());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->get(0).get_index());
        CHECK_EQUAL(2, origin_2->get_linklist(2, 1)->size());
        CHECK_EQUAL(0, origin_2->get_linklist(2, 1)->get(0).get_index());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 1)->get(1).get_index());
        CHECK(link_list_2_2->is_attached());
        CHECK_EQUAL(link_list_2_2, origin_2->get_linklist(2, 2));
        CHECK_EQUAL(2, link_list_2_2->size());
        CHECK_EQUAL(0, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_2->get(1).get_index());
        CHECK_EQUAL(1, origin_2->get_link(4, 0));
        CHECK_EQUAL(0, origin_2->get_link(4, 1));
        CHECK_EQUAL(1, origin_2->get_link(4, 2));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(3, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 4));
    }

    // Check that a link list can be cleared, and that a link can be moved
    // inside a link list
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        LinkViewRef link_list_1_2_w = origin_1_w->get_linklist(4, 2);
        LinkViewRef link_list_2_2_w = origin_2_w->get_linklist(2, 2);
        link_list_1_2_w->clear();    // Remove  O_1_LL_1[2] -> T_1[1]
        link_list_2_2_w->move(0, 1); // [ 0, 1 ] -> [ 1, 0 ]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // null       T_2[0]     []                     T_1[1]     [ T_2[1] ]             T_2[1]
    // T_1[0]     T_2[1]     [ T_1[0] ]             null       [ T_2[0], T_2[1] ]     T_2[0]
    // T_1[1]     null       []                     null       [ T_2[1], T_2[0] ]     T_2[1]
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK(origin_1->is_null_link(0, 0));
        CHECK_EQUAL(0, origin_1->get_link(0, 1));
        CHECK_EQUAL(1, origin_1->get_link(0, 2));
        CHECK_EQUAL(0, origin_1->get_link(2, 0));
        CHECK_EQUAL(1, origin_1->get_link(2, 1));
        CHECK(origin_1->is_null_link(2, 2));
        CHECK_EQUAL(0, origin_1->get_linklist(4, 0)->size());
        CHECK_EQUAL(1, origin_1->get_linklist(4, 1)->size());
        CHECK_EQUAL(0, origin_1->get_linklist(4, 1)->get(0).get_index());
        CHECK(link_list_1_2->is_attached());
        CHECK_EQUAL(link_list_1_2, origin_1->get_linklist(4, 2));
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK_EQUAL(1, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK(origin_2->is_null_link(0, 2));
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->size());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->get(0).get_index());
        CHECK_EQUAL(2, origin_2->get_linklist(2, 1)->size());
        CHECK_EQUAL(0, origin_2->get_linklist(2, 1)->get(0).get_index());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 1)->get(1).get_index());
        CHECK(link_list_2_2->is_attached());
        CHECK_EQUAL(link_list_2_2, origin_2->get_linklist(2, 2));
        CHECK_EQUAL(2, link_list_2_2->size());
        CHECK_EQUAL(1, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(0, link_list_2_2->get(1).get_index());
        CHECK_EQUAL(1, origin_2->get_link(4, 0));
        CHECK_EQUAL(0, origin_2->get_link(4, 1));
        CHECK_EQUAL(1, origin_2->get_link(4, 2));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(3, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 4));
    }

    // Check that a link list can have members swapped
    {
        WriteTransaction wt(sg_1);
        TableRef origin_2_w = wt.get_table("origin_2");
        LinkViewRef link_list_2_2_w = origin_2_w->get_linklist(2, 2);
        link_list_2_2_w->swap(0, 1); // [ 1, 0 ] -> [ 0, 1 ]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // null       T_2[0]     []                     T_1[1]     [ T_2[1] ]             T_2[1]
    // T_1[0]     T_2[1]     [ T_1[0] ]             null       [ T_2[0], T_2[1] ]     T_2[0]
    // T_1[1]     null       []                     null       [ T_2[0], T_2[1] ]     T_2[1]
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK(origin_1->is_null_link(0, 0));
        CHECK_EQUAL(0, origin_1->get_link(0, 1));
        CHECK_EQUAL(1, origin_1->get_link(0, 2));
        CHECK_EQUAL(0, origin_1->get_link(2, 0));
        CHECK_EQUAL(1, origin_1->get_link(2, 1));
        CHECK(origin_1->is_null_link(2, 2));
        CHECK_EQUAL(0, origin_1->get_linklist(4, 0)->size());
        CHECK_EQUAL(1, origin_1->get_linklist(4, 1)->size());
        CHECK_EQUAL(0, origin_1->get_linklist(4, 1)->get(0).get_index());
        CHECK(link_list_1_2->is_attached());
        CHECK_EQUAL(link_list_1_2, origin_1->get_linklist(4, 2));
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK_EQUAL(1, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK(origin_2->is_null_link(0, 2));
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->size());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->get(0).get_index());
        CHECK_EQUAL(2, origin_2->get_linklist(2, 1)->size());
        CHECK_EQUAL(0, origin_2->get_linklist(2, 1)->get(0).get_index());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 1)->get(1).get_index());
        CHECK(link_list_2_2->is_attached());
        CHECK_EQUAL(link_list_2_2, origin_2->get_linklist(2, 2));
        CHECK_EQUAL(2, link_list_2_2->size());
        CHECK_EQUAL(0, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_2->get(1).get_index());
        CHECK_EQUAL(1, origin_2->get_link(4, 0));
        CHECK_EQUAL(0, origin_2->get_link(4, 1));
        CHECK_EQUAL(1, origin_2->get_link(4, 2));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(3, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 4));
    }

    // Check that a link list can "swap" a member with itself
    {
        WriteTransaction wt(sg_1);
        TableRef origin_2_w = wt.get_table("origin_2");
        LinkViewRef link_list_2_2_w = origin_2_w->get_linklist(2, 2);
        link_list_2_2_w->swap(1, 1); // [ 0, 1 ] -> [ 0, 1 ]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // null       T_2[0]     []                     T_1[1]     [ T_2[1] ]             T_2[1]
    // T_1[0]     T_2[1]     [ T_1[0] ]             null       [ T_2[0], T_2[1] ]     T_2[0]
    // T_1[1]     null       []                     null       [ T_2[0], T_2[1] ]     T_2[1]
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK(origin_1->is_null_link(0, 0));
        CHECK_EQUAL(0, origin_1->get_link(0, 1));
        CHECK_EQUAL(1, origin_1->get_link(0, 2));
        CHECK_EQUAL(0, origin_1->get_link(2, 0));
        CHECK_EQUAL(1, origin_1->get_link(2, 1));
        CHECK(origin_1->is_null_link(2, 2));
        CHECK_EQUAL(0, origin_1->get_linklist(4, 0)->size());
        CHECK_EQUAL(1, origin_1->get_linklist(4, 1)->size());
        CHECK_EQUAL(0, origin_1->get_linklist(4, 1)->get(0).get_index());
        CHECK(link_list_1_2->is_attached());
        CHECK_EQUAL(link_list_1_2, origin_1->get_linklist(4, 2));
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK_EQUAL(1, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK(origin_2->is_null_link(0, 2));
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->size());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 0)->get(0).get_index());
        CHECK_EQUAL(2, origin_2->get_linklist(2, 1)->size());
        CHECK_EQUAL(0, origin_2->get_linklist(2, 1)->get(0).get_index());
        CHECK_EQUAL(1, origin_2->get_linklist(2, 1)->get(1).get_index());
        CHECK(link_list_2_2->is_attached());
        CHECK_EQUAL(link_list_2_2, origin_2->get_linklist(2, 2));
        CHECK_EQUAL(2, link_list_2_2->size());
        CHECK_EQUAL(0, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_2->get(1).get_index());
        CHECK_EQUAL(1, origin_2->get_link(4, 0));
        CHECK_EQUAL(0, origin_2->get_link(4, 1));
        CHECK_EQUAL(1, origin_2->get_link(4, 2));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(3, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 4));
    }

    // Reset to the state before testing swap
    {
        WriteTransaction wt(sg_1);
        TableRef origin_2_w = wt.get_table("origin_2");
        LinkViewRef link_list_2_2_w = origin_2_w->get_linklist(2, 2);
        link_list_2_2_w->swap(0, 1); // [ 0, 1 ] -> [ 1, 0 ]
        wt.commit();
    }
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // null       T_2[0]     []                     T_1[1]     [ T_2[1] ]             T_2[1]
    // T_1[0]     T_2[1]     [ T_1[0] ]             null       [ T_2[0], T_2[1] ]     T_2[0]
    // T_1[1]     null       []                     null       [ T_2[1], T_2[0] ]     T_2[1]

    // Check that an origin-side row can be deleted by a "move last over"
    // operation
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        origin_1_w->move_last_over(0); // [ 0, 1, 2 ] -> [ 2, 1 ]
        origin_2_w->move_last_over(2); // [ 0, 1, 2 ] -> [ 0, 1 ]
        // Removes  O_1_L_4[0]  -> T_2[0]  and  O_1_L_3[2]  -> T_1[1]  and
        //          O_2_LL_3[2] -> T_2[0]  and  O_2_LL_3[2] -> T_2[1]  and  O_2_L_4[2] -> T_2[1]
        // Adds     O_1_L_3[0]  -> T_1[1]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // T_1[1]     null       []                     T_1[1]     [ T_2[1] ]             T_2[1]
    // T_1[0]     T_2[1]     [ T_1[0] ]             null       [ T_2[0], T_2[1] ]     T_2[0]
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(2, origin_1->size());
        CHECK_EQUAL(2, origin_2->size());
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(4, 1);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(2, 1);
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK_EQUAL(0, origin_1->get_link(0, 1));
        CHECK(origin_1->is_null_link(2, 0));
        CHECK_EQUAL(1, origin_1->get_link(2, 1));
        CHECK_EQUAL(0, link_list_1_0->size());
        CHECK_EQUAL(1, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(1, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, link_list_2_0->size());
        CHECK_EQUAL(1, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(2, link_list_2_1->size());
        CHECK_EQUAL(0, link_list_2_1->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_1->get(1).get_index());
        CHECK_EQUAL(1, origin_2->get_link(4, 0));
        CHECK_EQUAL(0, origin_2->get_link(4, 1));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(0, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        origin_1_w->add_empty_row(); // [ 2, 1 ] -> [ 2, 1, 3 ]
        origin_1_w->set_link(2, 2, 0);
        origin_2_w->move_last_over(0); // [ 0, 1 ] -> [ 1 ]
        // Removes  O_2_L_2[0]  -> T_1[1]  and  O_2_LL_3[1] -> T_2[0]  and
        //          O_2_LL_3[1] -> T_2[1]  and  O_2_L_4[0]  -> T_2[1]  and  O_2_L_4[1] -> T_2[0]
        // Adds     O_1_L_4[2]  -> T_2[0]  and  O_2_LL_3[0] -> T_2[0]  and  O_2_L_4[0] -> T_2[0]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // T_1[1]     null       []                     null       [ T_2[0], T_2[1] ]     T_2[0]
    // T_1[0]     T_2[1]     [ T_1[0] ]
    // null       T_2[0]     []
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(3, origin_1->size());
        CHECK_EQUAL(1, origin_2->size());
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(4, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK_EQUAL(0, origin_1->get_link(0, 1));
        CHECK(origin_1->is_null_link(0, 2));
        CHECK(origin_1->is_null_link(2, 0));
        CHECK_EQUAL(1, origin_1->get_link(2, 1));
        CHECK_EQUAL(0, origin_1->get_link(2, 2));
        CHECK_EQUAL(0, link_list_1_0->size());
        CHECK_EQUAL(1, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK(origin_2->is_null_link(0, 0));
        CHECK_EQUAL(2, link_list_2_0->size());
        CHECK_EQUAL(0, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_0->get(1).get_index());
        CHECK_EQUAL(0, origin_2->get_link(4, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(1, *origin_2, 4));
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        origin_1_w->move_last_over(1); // [ 2, 1, 3 ] -> [ 2, 3 ]
        origin_2_w->move_last_over(0); // [ 1 ] -> []
        // Removes  O_1_L_3[1]  -> T_1[0]  and  O_1_L_4[1]  -> T_2[1]  and
        //          O_1_LL_1[1] -> T_1[0]  and  O_1_L_4[2]  -> T_2[0]  and
        //          O_2_LL_3[0] -> T_2[0]  and  O_2_LL_3[0] -> T_2[1]  and  O_2_L_4[0]  -> T_2[0]
        // Adds     O_1_L_4[1]  -> T_2[0]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // T_1[1]     null       []
    // null       T_2[0]     []
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(2, origin_1->size());
        CHECK_EQUAL(0, origin_2->size());
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(4, 1);
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK(origin_1->is_null_link(2, 0));
        CHECK_EQUAL(0, origin_1->get_link(2, 1));
        CHECK_EQUAL(0, link_list_1_0->size());
        CHECK_EQUAL(0, link_list_1_1->size());
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(0, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(1, *origin_2, 4));
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        origin_1_w->move_last_over(1); // [ 2, 3 ] -> [ 2 ]
        // Removes  O_1_L_4[1] -> T_2[0]
        origin_2_w->add_empty_row(3);           // [] -> [ 3, 4, 5 ]
        origin_2_w->set_link(0, 0, 0);          // O_2_L_2[0]  -> T_1[0]
        origin_2_w->set_link(0, 2, 1);          // O_2_L_2[2]  -> T_1[1]
        origin_2_w->get_linklist(2, 0)->add(1); // O_2_LL_3[0] -> T_2[1]
        origin_2_w->get_linklist(2, 1)->add(0); // O_2_LL_3[1] -> T_2[0]
        origin_2_w->get_linklist(2, 1)->add(1); // O_2_LL_3[1] -> T_2[1]
        origin_2_w->get_linklist(2, 2)->add(1); // O_2_LL_3[2] -> T_2[1]
        origin_2_w->get_linklist(2, 2)->add(0); // O_2_LL_3[2] -> T_2[0]
        origin_2_w->set_link(4, 0, 1);          // O_2_L_4[0]  -> T_2[1]
        origin_2_w->set_link(4, 2, 0);          // O_2_L_4[2]  -> T_2[0]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // T_1[1]     null       []                     T_1[0]     [ T_2[1] ]             T_2[1]
    //                                              null       [ T_2[0], T_2[1] ]     null
    //                                              T_1[1]     [ T_2[1], T_2[0] ]     T_2[0]
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(1, origin_1->size());
        CHECK_EQUAL(3, origin_2->size());
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(2, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK(origin_1->is_null_link(2, 0));
        CHECK_EQUAL(0, link_list_1_0->size());
        CHECK_EQUAL(0, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_2->get_link(0, 2));
        CHECK_EQUAL(1, link_list_2_0->size());
        CHECK_EQUAL(1, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(2, link_list_2_1->size());
        CHECK_EQUAL(0, link_list_2_1->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_1->get(1).get_index());
        CHECK_EQUAL(2, link_list_2_2->size());
        CHECK_EQUAL(1, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(0, link_list_2_2->get(1).get_index());
        CHECK_EQUAL(1, origin_2->get_link(4, 0));
        CHECK(origin_2->is_null_link(4, 1));
        CHECK_EQUAL(0, origin_2->get_link(4, 2));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(0, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(0, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(3, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        origin_1_w->add_empty_row(2);           // [ 2 ] -> [ 2, 4, 5 ]
        origin_1_w->set_link(0, 2, 0);          // O_1_L_3[2] -> T_1[0]
        origin_1_w->set_link(2, 0, 1);          // O_1_L_4[0] -> T_2[1]
        origin_1_w->set_link(2, 2, 0);          // O_1_L_4[2] -> T_2[0]
        origin_1_w->get_linklist(4, 1)->add(0); // O_1_LL_1[1] -> T_1[0]
        origin_1_w->get_linklist(4, 1)->add(0); // O_1_LL_1[1] -> T_1[0] (double)
        origin_1_w->get_linklist(4, 2)->add(1); // O_1_LL_1[2] -> T_1[1]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // T_1[1]     T_2[1]     []                     T_1[0]     [ T_2[1] ]             T_2[1]
    // null       null       [ T_1[0], T_1[0] ]     null       [ T_2[0], T_2[1] ]     null
    // T_1[0]     T_2[0]     [ T_1[1] ]             T_1[1]     [ T_2[1], T_2[0] ]     T_2[0]
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(3, origin_1->size());
        CHECK_EQUAL(3, origin_2->size());
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(4, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(2, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK_EQUAL(0, origin_1->get_link(0, 2));
        CHECK_EQUAL(1, origin_1->get_link(2, 0));
        CHECK(origin_1->is_null_link(2, 1));
        CHECK_EQUAL(0, origin_1->get_link(2, 2));
        CHECK_EQUAL(0, link_list_1_0->size());
        CHECK_EQUAL(2, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_1->get(1).get_index());
        CHECK_EQUAL(1, link_list_1_2->size());
        CHECK_EQUAL(1, link_list_1_2->get(0).get_index());
        CHECK_EQUAL(0, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_2->get_link(0, 2));
        CHECK_EQUAL(1, link_list_2_0->size());
        CHECK_EQUAL(1, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(2, link_list_2_1->size());
        CHECK_EQUAL(0, link_list_2_1->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_1->get(1).get_index());
        CHECK_EQUAL(2, link_list_2_2->size());
        CHECK_EQUAL(1, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(0, link_list_2_2->get(1).get_index());
        CHECK_EQUAL(1, origin_2->get_link(4, 0));
        CHECK(origin_2->is_null_link(4, 1));
        CHECK_EQUAL(0, origin_2->get_link(4, 2));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(2, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(3, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
    }

    // Check that an target-side row can be deleted by a "move last over"
    // operation
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        TableRef target_2_w = wt.get_table("target_2");
        target_2_w->add_empty_row();
        origin_1_w->get_linklist(4, 1)->set(0, 2);
        origin_2_w->get_linklist(2, 2)->set(1, 2);
        origin_2_w->set_link(4, 0, 2);
        // Removes  O_1_LL_1[1] -> T_1[0]  and  O_2_LL_3[2] -> T_2[0]  and  O_2_L_4[0] -> T_2[1]
        // Adds     O_1_LL_1[1] -> T_1[2]  and  O_2_LL_3[2] -> T_2[2]  and  O_2_L_4[0] -> T_2[2]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // T_1[1]     T_2[1]     []                     T_1[0]     [ T_2[1] ]             T_2[2]
    // null       null       [ T_1[2], T_1[0] ]     null       [ T_2[0], T_2[1] ]     null
    // T_1[0]     T_2[0]     [ T_1[1] ]             T_1[1]     [ T_2[1], T_2[2] ]     T_2[0]
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(3, origin_1->size());
        CHECK_EQUAL(3, origin_2->size());
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(4, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(2, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK_EQUAL(0, origin_1->get_link(0, 2));
        CHECK_EQUAL(1, origin_1->get_link(2, 0));
        CHECK(origin_1->is_null_link(2, 1));
        CHECK_EQUAL(0, origin_1->get_link(2, 2));
        CHECK_EQUAL(0, link_list_1_0->size());
        CHECK_EQUAL(2, link_list_1_1->size());
        CHECK_EQUAL(2, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_1->get(1).get_index());
        CHECK_EQUAL(1, link_list_1_2->size());
        CHECK_EQUAL(1, link_list_1_2->get(0).get_index());
        CHECK_EQUAL(0, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_2->get_link(0, 2));
        CHECK_EQUAL(1, link_list_2_0->size());
        CHECK_EQUAL(1, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(2, link_list_2_1->size());
        CHECK_EQUAL(0, link_list_2_1->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_1->get(1).get_index());
        CHECK_EQUAL(2, link_list_2_2->size());
        CHECK_EQUAL(1, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(2, link_list_2_2->get(1).get_index());
        CHECK_EQUAL(2, origin_2->get_link(4, 0));
        CHECK(origin_2->is_null_link(4, 1));
        CHECK_EQUAL(0, origin_2->get_link(4, 2));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(3, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(1, *origin_2, 4));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(2, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(2, *origin_2, 4));
    }
    {
        WriteTransaction wt(sg_1);
        TableRef target_1_w = wt.get_table("target_1");
        TableRef target_2_w = wt.get_table("target_2");
        target_1_w->move_last_over(0); // [ 0, 1, 2 ] -> [ 2, 1 ]
        target_2_w->move_last_over(2); // [ 0, 1, 2 ] -> [ 0, 1 ]
        // Removes  O_1_L_3[2] -> T_1[0]  and  O_1_LL_1[1] -> T_1[2]  and
        //          O_2_L_2[0] -> T_1[0]  and  O_2_LL_3[2] -> T_2[2]  and  O_2_L_4[0] -> T_2[2]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // T_1[1]     T_2[1]     []                     null       [ T_2[1] ]             null
    // null       null       [ T_1[0] ]             null       [ T_2[0], T_2[1] ]     null
    // null       T_2[0]     [ T_1[1] ]             T_1[1]     [ T_2[1] ]             T_2[0]
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(3, origin_1->size());
        CHECK_EQUAL(3, origin_2->size());
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(4, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(2, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK_EQUAL(2, target_1->size());
        CHECK_EQUAL(2, target_2->size());
        CHECK(link_list_1_0->is_attached());
        CHECK(link_list_1_1->is_attached());
        CHECK(link_list_1_2->is_attached());
        CHECK(link_list_2_0->is_attached());
        CHECK(link_list_2_1->is_attached());
        CHECK(link_list_2_2->is_attached());
        CHECK_EQUAL(link_list_1_0, origin_1->get_linklist(4, 0));
        CHECK_EQUAL(link_list_1_1, origin_1->get_linklist(4, 1));
        CHECK_EQUAL(link_list_1_2, origin_1->get_linklist(4, 2));
        CHECK_EQUAL(link_list_2_0, origin_2->get_linklist(2, 0));
        CHECK_EQUAL(link_list_2_1, origin_2->get_linklist(2, 1));
        CHECK_EQUAL(link_list_2_2, origin_2->get_linklist(2, 2));
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK(origin_1->is_null_link(0, 2));
        CHECK_EQUAL(1, origin_1->get_link(2, 0));
        CHECK(origin_1->is_null_link(2, 1));
        CHECK_EQUAL(0, origin_1->get_link(2, 2));
        CHECK_EQUAL(0, link_list_1_0->size());
        CHECK_EQUAL(1, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(1, link_list_1_2->size());
        CHECK_EQUAL(1, link_list_1_2->get(0).get_index());
        CHECK(origin_2->is_null_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_2->get_link(0, 2));
        CHECK_EQUAL(1, link_list_2_0->size());
        CHECK_EQUAL(1, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(2, link_list_2_1->size());
        CHECK_EQUAL(0, link_list_2_1->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_1->get(1).get_index());
        CHECK_EQUAL(1, link_list_2_2->size());
        CHECK_EQUAL(1, link_list_2_2->get(0).get_index());
        CHECK(origin_2->is_null_link(4, 0));
        CHECK(origin_2->is_null_link(4, 1));
        CHECK_EQUAL(0, origin_2->get_link(4, 2));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(3, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(1, *origin_2, 4));
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        TableRef target_1_w = wt.get_table("target_1");
        TableRef target_2_w = wt.get_table("target_2");
        target_1_w->add_empty_row();            // [ 2, 1 ] -> [ 2, 1, 3 ]
        origin_1_w->set_link(0, 2, 2);          // O_1_L_3[2]  -> T_1[2]
        origin_1_w->get_linklist(4, 1)->add(2); // O_1_LL_1[1] -> T_1[2]
        origin_2_w->set_link(0, 0, 2);          // O_2_L_2[0]  -> T_1[2]
        target_2_w->move_last_over(0);          // [ 0, 1 ] -> [ 1 ]
        // Removes  O_1_L_4[0]  -> T_2[1]  and  O_1_L_4[2]  -> T_2[0]  and
        //          O_2_LL_3[0] -> T_2[1]  and  O_2_LL_3[1] -> T_2[1]  and
        //          O_2_LL_3[2] -> T_2[1]  and  O_2_L_4[2]  -> T_2[0]
        // Adds     O_1_L_4[0]  -> T_2[0]  and  O_2_LL_3[0] -> T_2[0]  and
        //          O_2_LL_3[2] -> T_2[0]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // T_1[1]     T_2[0]     []                     T_1[2]     [ T_2[0] ]             null
    // null       null       [ T_1[0], T_1[2] ]     null       [ T_2[0] ]             null
    // T_1[2]     null       [ T_1[1] ]             T_1[1]     [ T_2[0] ]             null
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(3, origin_1->size());
        CHECK_EQUAL(3, origin_2->size());
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(4, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(2, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK_EQUAL(3, target_1->size());
        CHECK_EQUAL(1, target_2->size());
        CHECK(link_list_1_0->is_attached());
        CHECK(link_list_1_1->is_attached());
        CHECK(link_list_1_2->is_attached());
        CHECK(link_list_2_0->is_attached());
        CHECK(link_list_2_1->is_attached());
        CHECK(link_list_2_2->is_attached());
        CHECK_EQUAL(link_list_1_0, origin_1->get_linklist(4, 0));
        CHECK_EQUAL(link_list_1_1, origin_1->get_linklist(4, 1));
        CHECK_EQUAL(link_list_1_2, origin_1->get_linklist(4, 2));
        CHECK_EQUAL(link_list_2_0, origin_2->get_linklist(2, 0));
        CHECK_EQUAL(link_list_2_1, origin_2->get_linklist(2, 1));
        CHECK_EQUAL(link_list_2_2, origin_2->get_linklist(2, 2));
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK_EQUAL(2, origin_1->get_link(0, 2));
        CHECK_EQUAL(0, origin_1->get_link(2, 0));
        CHECK(origin_1->is_null_link(2, 1));
        CHECK(origin_1->is_null_link(2, 2));
        CHECK_EQUAL(0, link_list_1_0->size());
        CHECK_EQUAL(2, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(2, link_list_1_1->get(1).get_index());
        CHECK_EQUAL(1, link_list_1_2->size());
        CHECK_EQUAL(1, link_list_1_2->get(0).get_index());
        CHECK_EQUAL(2, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_2->get_link(0, 2));
        CHECK_EQUAL(1, link_list_2_0->size());
        CHECK_EQUAL(0, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_1->size());
        CHECK_EQUAL(0, link_list_2_1->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_2->size());
        CHECK_EQUAL(0, link_list_2_2->get(0).get_index());
        CHECK(origin_2->is_null_link(4, 0));
        CHECK(origin_2->is_null_link(4, 1));
        CHECK(origin_2->is_null_link(4, 2));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(3, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(0, *origin_2, 4));
    }
    {
        WriteTransaction wt(sg_1);
        TableRef target_1_w = wt.get_table("target_1");
        TableRef target_2_w = wt.get_table("target_2");
        target_1_w->move_last_over(1); // [ 2, 1, 3 ] -> [ 2, 3 ]
        target_2_w->move_last_over(0); // [ 1 ] -> []
        // Removes  O_1_L_3[0]  -> T_1[1]  and  O_1_L_3[2]  -> T_1[2]  and
        //          O_1_L_4[0]  -> T_2[0]  and  O_1_LL_1[1] -> T_1[2]  and
        //          O_1_LL_1[2] -> T_1[1]  and  O_2_L_2[0]  -> T_1[2]  and
        //          O_2_L_2[2]  -> T_1[1]  and  O_2_LL_3[0] -> T_2[0]  and
        //          O_2_LL_3[1] -> T_2[0]  and  O_2_LL_3[2] -> T_2[0]
        // Adds     O_1_L_3[2]  -> T_1[1]  and  O_1_LL_1[1] -> T_1[1]  and
        //          O_2_L_2[0]  -> T_1[1]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // null       null       []                     T_1[1]     []                     null
    // null       null       [ T_1[0], T_1[1] ]     null       []                     null
    // T_1[1]     null       []                     null       []                     null
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(3, origin_1->size());
        CHECK_EQUAL(3, origin_2->size());
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(4, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(2, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK_EQUAL(2, target_1->size());
        CHECK_EQUAL(0, target_2->size());
        CHECK(link_list_1_0->is_attached());
        CHECK(link_list_1_1->is_attached());
        CHECK(link_list_1_2->is_attached());
        CHECK(link_list_2_0->is_attached());
        CHECK(link_list_2_1->is_attached());
        CHECK(link_list_2_2->is_attached());
        CHECK_EQUAL(link_list_1_0, origin_1->get_linklist(4, 0));
        CHECK_EQUAL(link_list_1_1, origin_1->get_linklist(4, 1));
        CHECK_EQUAL(link_list_1_2, origin_1->get_linklist(4, 2));
        CHECK_EQUAL(link_list_2_0, origin_2->get_linklist(2, 0));
        CHECK_EQUAL(link_list_2_1, origin_2->get_linklist(2, 1));
        CHECK_EQUAL(link_list_2_2, origin_2->get_linklist(2, 2));
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK(origin_1->is_null_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_1->get_link(0, 2));
        CHECK(origin_1->is_null_link(2, 0));
        CHECK(origin_1->is_null_link(2, 1));
        CHECK(origin_1->is_null_link(2, 2));
        CHECK_EQUAL(0, link_list_1_0->size());
        CHECK_EQUAL(2, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(1, link_list_1_1->get(1).get_index());
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK_EQUAL(1, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK(origin_2->is_null_link(0, 2));
        CHECK_EQUAL(0, link_list_2_0->size());
        CHECK_EQUAL(0, link_list_2_1->size());
        CHECK_EQUAL(0, link_list_2_2->size());
        CHECK(origin_2->is_null_link(4, 0));
        CHECK(origin_2->is_null_link(4, 1));
        CHECK(origin_2->is_null_link(4, 2));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        TableRef target_1_w = wt.get_table("target_1");
        TableRef target_2_w = wt.get_table("target_2");
        target_1_w->move_last_over(1); // [ 2, 3 ] -> [ 2 ]
        // Removes  O_1_L_3[2] -> T_1[1]  and  O_1_LL_1[1] -> T_1[1]  and  O_2_L_2[0] -> T_1[1]
        target_2_w->add_empty_row(3);           // [] -> [ 3, 4, 5 ]
        origin_1_w->set_link(2, 0, 1);          // O_1_L_4[0]  -> T_2[1]
        origin_1_w->set_link(2, 2, 0);          // O_1_L_4[2]  -> T_2[0]
        origin_2_w->get_linklist(2, 0)->add(1); // O_2_LL_3[0] -> T_2[1]
        origin_2_w->get_linklist(2, 0)->add(1); // O_2_LL_3[0] -> T_2[1]
        origin_2_w->get_linklist(2, 2)->add(0); // O_2_LL_3[2] -> T_2[0]
        origin_2_w->set_link(4, 0, 0);          // O_2_L_4[0]  -> T_2[0]
        origin_2_w->set_link(4, 1, 1);          // O_2_L_4[1]  -> T_2[1]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // null       T_2[1]     []                     null       [ T_2[1], T_2[1] ]     T_2[0]
    // null       null       [ T_1[0] ]             null       []                     T_2[1]
    // null       T_2[0]     []                     null       [ T_2[0] ]             null
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(3, origin_1->size());
        CHECK_EQUAL(3, origin_2->size());
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(4, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(2, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK_EQUAL(1, target_1->size());
        CHECK_EQUAL(3, target_2->size());
        CHECK(link_list_1_0->is_attached());
        CHECK(link_list_1_1->is_attached());
        CHECK(link_list_1_2->is_attached());
        CHECK(link_list_2_0->is_attached());
        CHECK(link_list_2_1->is_attached());
        CHECK(link_list_2_2->is_attached());
        CHECK_EQUAL(link_list_1_0, origin_1->get_linklist(4, 0));
        CHECK_EQUAL(link_list_1_1, origin_1->get_linklist(4, 1));
        CHECK_EQUAL(link_list_1_2, origin_1->get_linklist(4, 2));
        CHECK_EQUAL(link_list_2_0, origin_2->get_linklist(2, 0));
        CHECK_EQUAL(link_list_2_1, origin_2->get_linklist(2, 1));
        CHECK_EQUAL(link_list_2_2, origin_2->get_linklist(2, 2));
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK(origin_1->is_null_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK(origin_1->is_null_link(0, 2));
        CHECK_EQUAL(1, origin_1->get_link(2, 0));
        CHECK(origin_1->is_null_link(2, 1));
        CHECK_EQUAL(0, origin_1->get_link(2, 2));
        CHECK_EQUAL(0, link_list_1_0->size());
        CHECK_EQUAL(1, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK(origin_2->is_null_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK(origin_2->is_null_link(0, 2));
        CHECK_EQUAL(2, link_list_2_0->size());
        CHECK_EQUAL(1, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_0->get(1).get_index());
        CHECK_EQUAL(0, link_list_2_1->size());
        CHECK_EQUAL(1, link_list_2_2->size());
        CHECK_EQUAL(0, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(0, origin_2->get_link(4, 0));
        CHECK_EQUAL(1, origin_2->get_link(4, 1));
        CHECK(origin_2->is_null_link(4, 2));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_1, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 4));
    }
    {
        WriteTransaction wt(sg_1);
        TableRef target_1_w = wt.get_table("target_1");
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        target_1_w->add_empty_row(2);           // [ 2 ] -> [ 2, 4, 5 ]
        origin_1_w->set_link(0, 0, 1);          // O_1_L_3[0] -> T_1[1]
        origin_1_w->set_link(0, 2, 0);          // O_1_L_3[2] -> T_1[0]
        origin_1_w->get_linklist(4, 0)->add(1); // O_1_LL_1[0] -> T_1[1]
        origin_1_w->get_linklist(4, 0)->add(0); // O_1_LL_1[0] -> T_1[0]
        origin_2_w->set_link(0, 0, 0);          // O_2_L_2[0] -> T_1[0]
        origin_2_w->set_link(0, 2, 1);          // O_2_L_2[2] -> T_1[1]
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // T_1[1]     T_2[1]     [ T_1[1], T_1[0] ]     T_1[0]     [ T_2[1], T_2[1] ]     T_2[0]
    // null       null       [ T_1[0] ]             null       []                     T_2[1]
    // T_1[0]     T_2[0]     []                     T_1[1]     [ T_2[0] ]             null
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(3, origin_1->size());
        CHECK_EQUAL(3, origin_2->size());
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(4, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(2, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK_EQUAL(3, target_1->size());
        CHECK_EQUAL(3, target_2->size());
        CHECK(link_list_1_0->is_attached());
        CHECK(link_list_1_1->is_attached());
        CHECK(link_list_1_2->is_attached());
        CHECK(link_list_2_0->is_attached());
        CHECK(link_list_2_1->is_attached());
        CHECK(link_list_2_2->is_attached());
        CHECK_EQUAL(link_list_1_0, origin_1->get_linklist(4, 0));
        CHECK_EQUAL(link_list_1_1, origin_1->get_linklist(4, 1));
        CHECK_EQUAL(link_list_1_2, origin_1->get_linklist(4, 2));
        CHECK_EQUAL(link_list_2_0, origin_2->get_linklist(2, 0));
        CHECK_EQUAL(link_list_2_1, origin_2->get_linklist(2, 1));
        CHECK_EQUAL(link_list_2_2, origin_2->get_linklist(2, 2));
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK_EQUAL(0, origin_1->get_link(0, 2));
        CHECK_EQUAL(1, origin_1->get_link(2, 0));
        CHECK(origin_1->is_null_link(2, 1));
        CHECK_EQUAL(0, origin_1->get_link(2, 2));
        CHECK_EQUAL(2, link_list_1_0->size());
        CHECK_EQUAL(1, link_list_1_0->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_0->get(1).get_index());
        CHECK_EQUAL(1, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK_EQUAL(0, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_2->get_link(0, 2));
        CHECK_EQUAL(2, link_list_2_0->size());
        CHECK_EQUAL(1, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_0->get(1).get_index());
        CHECK_EQUAL(0, link_list_2_1->size());
        CHECK_EQUAL(1, link_list_2_2->size());
        CHECK_EQUAL(0, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(0, origin_2->get_link(4, 0));
        CHECK_EQUAL(1, origin_2->get_link(4, 1));
        CHECK(origin_2->is_null_link(4, 2));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(2, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_1, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 4));
    }

    // Check that an origin-side table can be cleared
    {
        WriteTransaction wt(sg_1);
        TableRef origin_2_w = wt.get_table("origin_2");
        origin_2_w->clear();
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // T_1[1]     T_2[1]     [ T_1[1], T_1[0] ]
    // null       null       [ T_1[0] ]
    // T_1[0]     T_2[0]     []
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(3, origin_1->size());
        CHECK_EQUAL(0, origin_2->size());
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(4, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK_EQUAL(0, origin_1->get_link(0, 2));
        CHECK_EQUAL(1, origin_1->get_link(2, 0));
        CHECK(origin_1->is_null_link(2, 1));
        CHECK_EQUAL(0, origin_1->get_link(2, 2));
        CHECK_EQUAL(2, link_list_1_0->size());
        CHECK_EQUAL(1, link_list_1_0->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_0->get(1).get_index());
        CHECK_EQUAL(1, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(2, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(1, *origin_2, 4));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_1, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 4));
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_2_w = wt.get_table("origin_2");
        origin_2_w->add_empty_row(3);
        origin_2_w->set_link(0, 0, 0);
        origin_2_w->set_link(0, 2, 1);
        origin_2_w->get_linklist(2, 0)->add(1);
        origin_2_w->get_linklist(2, 0)->add(1);
        origin_2_w->get_linklist(2, 2)->add(0);
        origin_2_w->set_link(4, 0, 0);
        origin_2_w->set_link(4, 1, 1);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // T_1[1]     T_2[1]     [ T_1[1], T_1[0] ]     T_1[0]     [ T_2[1], T_2[1] ]     T_2[0]
    // null       null       [ T_1[0] ]             null       []                     T_2[1]
    // T_1[0]     T_2[0]     []                     T_1[1]     [ T_2[0] ]             null
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(3, origin_1->size());
        CHECK_EQUAL(3, origin_2->size());
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(4, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(2, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK_EQUAL(0, origin_1->get_link(0, 2));
        CHECK_EQUAL(1, origin_1->get_link(2, 0));
        CHECK(origin_1->is_null_link(2, 1));
        CHECK_EQUAL(0, origin_1->get_link(2, 2));
        CHECK_EQUAL(2, link_list_1_0->size());
        CHECK_EQUAL(1, link_list_1_0->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_0->get(1).get_index());
        CHECK_EQUAL(1, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK_EQUAL(0, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_2->get_link(0, 2));
        CHECK_EQUAL(2, link_list_2_0->size());
        CHECK_EQUAL(1, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_0->get(1).get_index());
        CHECK_EQUAL(0, link_list_2_1->size());
        CHECK_EQUAL(1, link_list_2_2->size());
        CHECK_EQUAL(0, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(0, origin_2->get_link(4, 0));
        CHECK_EQUAL(1, origin_2->get_link(4, 1));
        CHECK(origin_2->is_null_link(4, 2));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(2, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_1, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 4));
    }

    // Check that a target-side table can be cleared
    {
        WriteTransaction wt(sg_1);
        TableRef target_2_w = wt.get_table("target_2");
        target_2_w->clear();
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // T_1[1]     null       [ T_1[1], T_1[0] ]     T_1[0]     []                     null
    // null       null       [ T_1[0] ]             null       []                     null
    // T_1[0]     null       []                     T_1[1]     []                     null
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(3, origin_1->size());
        CHECK_EQUAL(3, origin_2->size());
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(4, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(2, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK_EQUAL(3, target_1->size());
        CHECK_EQUAL(0, target_2->size());
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK_EQUAL(0, origin_1->get_link(0, 2));
        CHECK(origin_1->is_null_link(2, 0));
        CHECK(origin_1->is_null_link(2, 1));
        CHECK(origin_1->is_null_link(2, 2));
        CHECK_EQUAL(2, link_list_1_0->size());
        CHECK_EQUAL(1, link_list_1_0->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_0->get(1).get_index());
        CHECK_EQUAL(1, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK_EQUAL(0, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_2->get_link(0, 2));
        CHECK_EQUAL(0, link_list_2_0->size());
        CHECK_EQUAL(0, link_list_2_1->size());
        CHECK_EQUAL(0, link_list_2_2->size());
        CHECK(origin_2->is_null_link(4, 0));
        CHECK(origin_2->is_null_link(4, 1));
        CHECK(origin_2->is_null_link(4, 2));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(2, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        TableRef target_2_w = wt.get_table("target_2");
        target_2_w->add_empty_row(3);
        origin_1_w->set_link(2, 0, 1);
        origin_1_w->set_link(2, 2, 0);
        origin_2_w->get_linklist(2, 0)->add(1);
        origin_2_w->get_linklist(2, 0)->add(1);
        origin_2_w->get_linklist(2, 2)->add(0);
        origin_2_w->set_link(4, 0, 0);
        origin_2_w->set_link(4, 1, 1);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    // O_1_L_3    O_1_L_4    O_1_LL_1               O_2_L_2    O_2_LL_3               O_2_L_4
    // ----------------------------------------------------------------------------------------
    // T_1[1]     T_2[1]     [ T_1[1], T_1[0] ]     T_1[0]     [ T_2[1], T_2[1] ]     T_2[0]
    // null       null       [ T_1[0] ]             null       []                     T_2[1]
    // T_1[0]     T_2[0]     []                     T_1[1]     [ T_2[0] ]             null
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(3, origin_1->size());
        CHECK_EQUAL(3, origin_2->size());
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(4, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(2, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK_EQUAL(3, target_1->size());
        CHECK_EQUAL(3, target_2->size());
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK_EQUAL(0, origin_1->get_link(0, 2));
        CHECK_EQUAL(1, origin_1->get_link(2, 0));
        CHECK(origin_1->is_null_link(2, 1));
        CHECK_EQUAL(0, origin_1->get_link(2, 2));
        CHECK_EQUAL(2, link_list_1_0->size());
        CHECK_EQUAL(1, link_list_1_0->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_0->get(1).get_index());
        CHECK_EQUAL(1, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK_EQUAL(0, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_2->get_link(0, 2));
        CHECK_EQUAL(2, link_list_2_0->size());
        CHECK_EQUAL(1, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_0->get(1).get_index());
        CHECK_EQUAL(0, link_list_2_1->size());
        CHECK_EQUAL(1, link_list_2_2->size());
        CHECK_EQUAL(0, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(0, origin_2->get_link(4, 0));
        CHECK_EQUAL(1, origin_2->get_link(4, 1));
        CHECK(origin_2->is_null_link(4, 2));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(2, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_1, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 4));
    }

    // Check that non-link columns can be inserted into origin table and removed
    // from it
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        CHECK_EQUAL(5, origin_1->get_column_count());
        CHECK_EQUAL(5, origin_2->get_column_count());
        CHECK_EQUAL(type_Link, origin_1->get_column_type(0));
        CHECK_EQUAL(type_Int, origin_1->get_column_type(1));
        CHECK_EQUAL(type_Link, origin_1->get_column_type(2));
        CHECK_EQUAL(type_Int, origin_1->get_column_type(3));
        CHECK_EQUAL(type_LinkList, origin_1->get_column_type(4));
        CHECK_EQUAL(type_Link, origin_2->get_column_type(0));
        CHECK_EQUAL(type_Int, origin_2->get_column_type(1));
        CHECK_EQUAL(type_LinkList, origin_2->get_column_type(2));
        CHECK_EQUAL(type_Int, origin_2->get_column_type(3));
        CHECK_EQUAL(type_Link, origin_2->get_column_type(4));
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        origin_1_w->insert_column(2, type_Table, "foo_1");
        origin_2_w->insert_column(0, type_Table, "foo_2");
        origin_2_w->insert_column(6, type_String, "foo_3");
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(6, origin_1->get_column_count());
        CHECK_EQUAL(7, origin_2->get_column_count());
        CHECK_EQUAL(type_Link, origin_1->get_column_type(0));
        CHECK_EQUAL(type_Int, origin_1->get_column_type(1));
        CHECK_EQUAL(type_Table, origin_1->get_column_type(2));
        CHECK_EQUAL(type_Link, origin_1->get_column_type(3));
        CHECK_EQUAL(type_Int, origin_1->get_column_type(4));
        CHECK_EQUAL(type_LinkList, origin_1->get_column_type(5));
        CHECK_EQUAL(type_Table, origin_2->get_column_type(0));
        CHECK_EQUAL(type_Link, origin_2->get_column_type(1));
        CHECK_EQUAL(type_Int, origin_2->get_column_type(2));
        CHECK_EQUAL(type_LinkList, origin_2->get_column_type(3));
        CHECK_EQUAL(type_Int, origin_2->get_column_type(4));
        CHECK_EQUAL(type_Link, origin_2->get_column_type(5));
        CHECK_EQUAL(type_String, origin_2->get_column_type(6));
        CHECK_EQUAL(3, origin_1->size());
        CHECK_EQUAL(3, origin_2->size());
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(5, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(5, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(5, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(3, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(3, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(3, 2);
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK_EQUAL(0, origin_1->get_link(0, 2));
        CHECK_EQUAL(1, origin_1->get_link(3, 0));
        CHECK(origin_1->is_null_link(3, 1));
        CHECK_EQUAL(0, origin_1->get_link(3, 2));
        CHECK_EQUAL(2, link_list_1_0->size());
        CHECK_EQUAL(1, link_list_1_0->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_0->get(1).get_index());
        CHECK_EQUAL(1, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK_EQUAL(0, origin_2->get_link(1, 0));
        CHECK(origin_2->is_null_link(1, 1));
        CHECK_EQUAL(1, origin_2->get_link(1, 2));
        CHECK_EQUAL(2, link_list_2_0->size());
        CHECK_EQUAL(1, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_0->get(1).get_index());
        CHECK_EQUAL(0, link_list_2_1->size());
        CHECK_EQUAL(1, link_list_2_2->size());
        CHECK_EQUAL(0, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(0, origin_2->get_link(5, 0));
        CHECK_EQUAL(1, origin_2->get_link(5, 1));
        CHECK(origin_2->is_null_link(5, 2));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(2, target_1->get_backlink_count(0, *origin_1, 5));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_2, 1));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 5));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 1));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 5));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 1));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 3));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 3));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 5));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 3));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 3));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 5));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_1, 3));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 3));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 5));
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        origin_1_w->insert_column(4, type_Mixed, "foo_4");
        origin_2_w->remove_column(0);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(7, origin_1->get_column_count());
        CHECK_EQUAL(6, origin_2->get_column_count());
        CHECK_EQUAL(type_Link, origin_1->get_column_type(0));
        CHECK_EQUAL(type_Int, origin_1->get_column_type(1));
        CHECK_EQUAL(type_Table, origin_1->get_column_type(2));
        CHECK_EQUAL(type_Link, origin_1->get_column_type(3));
        CHECK_EQUAL(type_Mixed, origin_1->get_column_type(4));
        CHECK_EQUAL(type_Int, origin_1->get_column_type(5));
        CHECK_EQUAL(type_LinkList, origin_1->get_column_type(6));
        CHECK_EQUAL(type_Link, origin_2->get_column_type(0));
        CHECK_EQUAL(type_Int, origin_2->get_column_type(1));
        CHECK_EQUAL(type_LinkList, origin_2->get_column_type(2));
        CHECK_EQUAL(type_Int, origin_2->get_column_type(3));
        CHECK_EQUAL(type_Link, origin_2->get_column_type(4));
        CHECK_EQUAL(type_String, origin_2->get_column_type(5));
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(6, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(6, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(6, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(2, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK_EQUAL(0, origin_1->get_link(0, 2));
        CHECK_EQUAL(1, origin_1->get_link(3, 0));
        CHECK(origin_1->is_null_link(3, 1));
        CHECK_EQUAL(0, origin_1->get_link(3, 2));
        CHECK_EQUAL(2, link_list_1_0->size());
        CHECK_EQUAL(1, link_list_1_0->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_0->get(1).get_index());
        CHECK_EQUAL(1, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK_EQUAL(0, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_2->get_link(0, 2));
        CHECK_EQUAL(2, link_list_2_0->size());
        CHECK_EQUAL(1, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_0->get(1).get_index());
        CHECK_EQUAL(0, link_list_2_1->size());
        CHECK_EQUAL(1, link_list_2_2->size());
        CHECK_EQUAL(0, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(0, origin_2->get_link(4, 0));
        CHECK_EQUAL(1, origin_2->get_link(4, 1));
        CHECK(origin_2->is_null_link(4, 2));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(2, target_1->get_backlink_count(0, *origin_1, 6));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 6));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 6));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 3));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 3));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_1, 3));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 4));
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        origin_1_w->remove_column(2);
        origin_1_w->remove_column(3);
        origin_2_w->remove_column(5);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(5, origin_1->get_column_count());
        CHECK_EQUAL(5, origin_2->get_column_count());
        CHECK_EQUAL(type_Link, origin_1->get_column_type(0));
        CHECK_EQUAL(type_Int, origin_1->get_column_type(1));
        CHECK_EQUAL(type_Link, origin_1->get_column_type(2));
        CHECK_EQUAL(type_Int, origin_1->get_column_type(3));
        CHECK_EQUAL(type_LinkList, origin_1->get_column_type(4));
        CHECK_EQUAL(type_Link, origin_2->get_column_type(0));
        CHECK_EQUAL(type_Int, origin_2->get_column_type(1));
        CHECK_EQUAL(type_LinkList, origin_2->get_column_type(2));
        CHECK_EQUAL(type_Int, origin_2->get_column_type(3));
        CHECK_EQUAL(type_Link, origin_2->get_column_type(4));
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(4, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(2, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK_EQUAL(0, origin_1->get_link(0, 2));
        CHECK_EQUAL(1, origin_1->get_link(2, 0));
        CHECK(origin_1->is_null_link(2, 1));
        CHECK_EQUAL(0, origin_1->get_link(2, 2));
        CHECK_EQUAL(2, link_list_1_0->size());
        CHECK_EQUAL(1, link_list_1_0->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_0->get(1).get_index());
        CHECK_EQUAL(1, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK_EQUAL(0, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_2->get_link(0, 2));
        CHECK_EQUAL(2, link_list_2_0->size());
        CHECK_EQUAL(1, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_0->get(1).get_index());
        CHECK_EQUAL(0, link_list_2_1->size());
        CHECK_EQUAL(1, link_list_2_2->size());
        CHECK_EQUAL(0, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(0, origin_2->get_link(4, 0));
        CHECK_EQUAL(1, origin_2->get_link(4, 1));
        CHECK(origin_2->is_null_link(4, 2));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(2, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_1, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 4));
    }

    // Check that link columns can be inserted into origin table and removed
    // from it
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        TableRef target_1_w = wt.get_table("target_1");
        TableRef target_2_w = wt.get_table("target_2");
        origin_1_w->insert_column_link(2, type_LinkList, "bar_1", *target_2_w);
        origin_2_w->insert_column_link(0, type_Link, "bar_2", *target_1_w);
        origin_2_w->insert_column_link(6, type_LinkList, "bar_3", *target_2_w);
        origin_2_w->set_link(0, 0, 2);
        origin_2_w->set_link(0, 1, 0);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(6, origin_1->get_column_count());
        CHECK_EQUAL(7, origin_2->get_column_count());
        CHECK_EQUAL(type_Link, origin_1->get_column_type(0));
        CHECK_EQUAL(type_Int, origin_1->get_column_type(1));
        CHECK_EQUAL(type_LinkList, origin_1->get_column_type(2));
        CHECK_EQUAL(type_Link, origin_1->get_column_type(3));
        CHECK_EQUAL(type_Int, origin_1->get_column_type(4));
        CHECK_EQUAL(type_LinkList, origin_1->get_column_type(5));
        CHECK_EQUAL(type_Link, origin_2->get_column_type(0));
        CHECK_EQUAL(type_Link, origin_2->get_column_type(1));
        CHECK_EQUAL(type_Int, origin_2->get_column_type(2));
        CHECK_EQUAL(type_LinkList, origin_2->get_column_type(3));
        CHECK_EQUAL(type_Int, origin_2->get_column_type(4));
        CHECK_EQUAL(type_Link, origin_2->get_column_type(5));
        CHECK_EQUAL(type_LinkList, origin_2->get_column_type(6));
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK_EQUAL(0, origin_1->get_link(0, 2));
        CHECK_EQUAL(1, origin_1->get_link(3, 0));
        CHECK(origin_1->is_null_link(3, 1));
        CHECK_EQUAL(0, origin_1->get_link(3, 2));
        CHECK_EQUAL(2, origin_2->get_link(0, 0));
        CHECK_EQUAL(0, origin_2->get_link(0, 1));
        CHECK(origin_2->is_null_link(0, 2));
        CHECK_EQUAL(0, origin_2->get_link(1, 0));
        CHECK(origin_2->is_null_link(1, 1));
        CHECK_EQUAL(1, origin_2->get_link(1, 2));
        CHECK_EQUAL(0, origin_2->get_link(5, 0));
        CHECK_EQUAL(1, origin_2->get_link(5, 1));
        CHECK(origin_2->is_null_link(5, 2));
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(5, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(5, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(5, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(3, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(3, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(3, 2);
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_0->size());
        CHECK_EQUAL(1, link_list_1_0->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_0->get(1).get_index());
        CHECK_EQUAL(1, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK_EQUAL(2, link_list_2_0->size());
        CHECK_EQUAL(1, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_0->get(1).get_index());
        CHECK_EQUAL(0, link_list_2_1->size());
        CHECK_EQUAL(1, link_list_2_2->size());
        CHECK_EQUAL(0, link_list_2_2->get(0).get_index());
        ConstLinkViewRef link_list_1_0_x = origin_1->get_linklist(2, 0);
        ConstLinkViewRef link_list_1_1_x = origin_1->get_linklist(2, 1);
        ConstLinkViewRef link_list_1_2_x = origin_1->get_linklist(2, 2);
        ConstLinkViewRef link_list_2_0_x = origin_2->get_linklist(6, 0);
        ConstLinkViewRef link_list_2_1_x = origin_2->get_linklist(6, 1);
        ConstLinkViewRef link_list_2_2_x = origin_2->get_linklist(6, 2);
        CHECK_EQUAL(0, link_list_1_0_x->size());
        CHECK_EQUAL(0, link_list_1_1_x->size());
        CHECK_EQUAL(0, link_list_1_2_x->size());
        CHECK_EQUAL(0, link_list_2_0_x->size());
        CHECK_EQUAL(0, link_list_2_1_x->size());
        CHECK_EQUAL(0, link_list_2_2_x->size());
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(2, target_1->get_backlink_count(0, *origin_1, 5));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_2, 1));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 5));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 1));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 5));
        CHECK_EQUAL(1, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 1));
        CHECK_EQUAL(0, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 3));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 3));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 5));
        CHECK_EQUAL(0, target_2->get_backlink_count(0, *origin_2, 6));
        CHECK_EQUAL(0, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 3));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 3));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 5));
        CHECK_EQUAL(0, target_2->get_backlink_count(1, *origin_2, 6));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_1, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_1, 3));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 3));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 5));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 6));
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        TableRef target_1_w = wt.get_table("target_1");
        origin_1_w->insert_column_link(4, type_Link, "bar_4", *target_1_w);
        origin_2_w->remove_column(0);
        origin_1_w->set_link(4, 1, 2);
        origin_1_w->set_link(4, 2, 0);
        origin_1_w->get_linklist(2, 1)->add(2);
        origin_1_w->get_linklist(2, 1)->add(1);
        origin_1_w->get_linklist(2, 1)->add(2);
        origin_1_w->get_linklist(2, 2)->add(1);
        origin_2_w->get_linklist(5, 0)->add(1);
        origin_2_w->get_linklist(5, 2)->add(0);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(7, origin_1->get_column_count());
        CHECK_EQUAL(6, origin_2->get_column_count());
        CHECK_EQUAL(type_Link, origin_1->get_column_type(0));
        CHECK_EQUAL(type_Int, origin_1->get_column_type(1));
        CHECK_EQUAL(type_LinkList, origin_1->get_column_type(2));
        CHECK_EQUAL(type_Link, origin_1->get_column_type(3));
        CHECK_EQUAL(type_Link, origin_1->get_column_type(4));
        CHECK_EQUAL(type_Int, origin_1->get_column_type(5));
        CHECK_EQUAL(type_LinkList, origin_1->get_column_type(6));
        CHECK_EQUAL(type_Link, origin_2->get_column_type(0));
        CHECK_EQUAL(type_Int, origin_2->get_column_type(1));
        CHECK_EQUAL(type_LinkList, origin_2->get_column_type(2));
        CHECK_EQUAL(type_Int, origin_2->get_column_type(3));
        CHECK_EQUAL(type_Link, origin_2->get_column_type(4));
        CHECK_EQUAL(type_LinkList, origin_2->get_column_type(5));
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK_EQUAL(0, origin_1->get_link(0, 2));
        CHECK_EQUAL(1, origin_1->get_link(3, 0));
        CHECK(origin_1->is_null_link(3, 1));
        CHECK_EQUAL(0, origin_1->get_link(3, 2));
        CHECK(origin_1->is_null_link(4, 0));
        CHECK_EQUAL(2, origin_1->get_link(4, 1));
        CHECK_EQUAL(0, origin_1->get_link(4, 2));
        CHECK_EQUAL(0, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_2->get_link(0, 2));
        CHECK_EQUAL(0, origin_2->get_link(4, 0));
        CHECK_EQUAL(1, origin_2->get_link(4, 1));
        CHECK(origin_2->is_null_link(4, 2));
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(6, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(6, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(6, 2);
        ConstLinkViewRef link_list_1_0_x = origin_1->get_linklist(2, 0);
        ConstLinkViewRef link_list_1_1_x = origin_1->get_linklist(2, 1);
        ConstLinkViewRef link_list_1_2_x = origin_1->get_linklist(2, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(2, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        ConstLinkViewRef link_list_2_0_x = origin_2->get_linklist(5, 0);
        ConstLinkViewRef link_list_2_1_x = origin_2->get_linklist(5, 1);
        ConstLinkViewRef link_list_2_2_x = origin_2->get_linklist(5, 2);
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_1_0_x->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1_x->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2_x->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0_x->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1_x->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2_x->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_0->size());
        CHECK_EQUAL(1, link_list_1_0->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_0->get(1).get_index());
        CHECK_EQUAL(1, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK_EQUAL(0, link_list_1_0_x->size());
        CHECK_EQUAL(3, link_list_1_1_x->size());
        CHECK_EQUAL(2, link_list_1_1_x->get(0).get_index());
        CHECK_EQUAL(1, link_list_1_1_x->get(1).get_index());
        CHECK_EQUAL(2, link_list_1_1_x->get(2).get_index());
        CHECK_EQUAL(1, link_list_1_2_x->size());
        CHECK_EQUAL(1, link_list_1_2_x->get(0).get_index());
        CHECK_EQUAL(2, link_list_2_0->size());
        CHECK_EQUAL(1, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_0->get(1).get_index());
        CHECK_EQUAL(0, link_list_2_1->size());
        CHECK_EQUAL(1, link_list_2_2->size());
        CHECK_EQUAL(0, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_0_x->size());
        CHECK_EQUAL(1, link_list_2_0_x->get(0).get_index());
        CHECK_EQUAL(0, link_list_2_1_x->size());
        CHECK_EQUAL(1, link_list_2_2_x->size());
        CHECK_EQUAL(0, link_list_2_2_x->get(0).get_index());
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(2, target_1->get_backlink_count(0, *origin_1, 6));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 6));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 6));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(0, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 3));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 5));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 3));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 5));
        CHECK_EQUAL(2, target_2->get_backlink_count(2, *origin_1, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_1, 3));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 4));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 5));
    }
    {
        WriteTransaction wt(sg_1);
        TableRef origin_1_w = wt.get_table("origin_1");
        TableRef origin_2_w = wt.get_table("origin_2");
        origin_1_w->remove_column(2);
        origin_1_w->remove_column(3);
        origin_2_w->remove_column(5);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(5, origin_1->get_column_count());
        CHECK_EQUAL(5, origin_2->get_column_count());
        CHECK_EQUAL(type_Link, origin_1->get_column_type(0));
        CHECK_EQUAL(type_Int, origin_1->get_column_type(1));
        CHECK_EQUAL(type_Link, origin_1->get_column_type(2));
        CHECK_EQUAL(type_Int, origin_1->get_column_type(3));
        CHECK_EQUAL(type_LinkList, origin_1->get_column_type(4));
        CHECK_EQUAL(type_Link, origin_2->get_column_type(0));
        CHECK_EQUAL(type_Int, origin_2->get_column_type(1));
        CHECK_EQUAL(type_LinkList, origin_2->get_column_type(2));
        CHECK_EQUAL(type_Int, origin_2->get_column_type(3));
        CHECK_EQUAL(type_Link, origin_2->get_column_type(4));
        CHECK_EQUAL(3, origin_1->size());
        CHECK_EQUAL(3, origin_2->size());
        CHECK_EQUAL(1, origin_1->get_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK_EQUAL(0, origin_1->get_link(0, 2));
        CHECK_EQUAL(1, origin_1->get_link(2, 0));
        CHECK(origin_1->is_null_link(2, 1));
        CHECK_EQUAL(0, origin_1->get_link(2, 2));
        CHECK_EQUAL(0, origin_2->get_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK_EQUAL(1, origin_2->get_link(0, 2));
        CHECK_EQUAL(0, origin_2->get_link(4, 0));
        CHECK_EQUAL(1, origin_2->get_link(4, 1));
        CHECK(origin_2->is_null_link(4, 2));
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(4, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(2, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_0->size());
        CHECK_EQUAL(1, link_list_1_0->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_0->get(1).get_index());
        CHECK_EQUAL(1, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_1->get(0).get_index());
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK_EQUAL(2, link_list_2_0->size());
        CHECK_EQUAL(1, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_0->get(1).get_index());
        CHECK_EQUAL(0, link_list_2_1->size());
        CHECK_EQUAL(1, link_list_2_2->size());
        CHECK_EQUAL(0, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(2, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_1, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 4));
    }

    // Check that columns can be inserted into target table and removed from it
    {
        WriteTransaction wt(sg_1);
        TableRef target_1_w = wt.get_table("target_1");
        TableRef target_2_w = wt.get_table("target_2");
        target_1_w->insert_column(0, type_Mixed, "t_3");
        target_2_w->insert_column_link(1, type_Link, "t_4", *target_1_w);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(2, target_1->get_column_count());
        CHECK_EQUAL(2, target_2->get_column_count());
        CHECK_EQUAL(type_Mixed, target_1->get_column_type(0));
        CHECK_EQUAL(type_Int, target_1->get_column_type(1));
        CHECK_EQUAL(type_Int, target_2->get_column_type(0));
        CHECK_EQUAL(type_Link, target_2->get_column_type(1));
        CHECK_EQUAL(3, target_1->size());
        CHECK_EQUAL(3, target_2->size());
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(2, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_1, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 4));
    }
    {
        WriteTransaction wt(sg_1);
        TableRef target_1_w = wt.get_table("target_1");
        TableRef target_2_w = wt.get_table("target_2");
        target_1_w->remove_column(1);
        target_2_w->remove_column(0);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(1, target_1->get_column_count());
        CHECK_EQUAL(1, target_2->get_column_count());
        CHECK_EQUAL(type_Mixed, target_1->get_column_type(0));
        CHECK_EQUAL(type_Link, target_2->get_column_type(0));
        CHECK_EQUAL(3, target_1->size());
        CHECK_EQUAL(3, target_2->size());
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_1, 0));
        CHECK_EQUAL(2, target_1->get_backlink_count(0, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(0, *origin_2, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 0));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_1, 4));
        CHECK_EQUAL(1, target_1->get_backlink_count(1, *origin_2, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 0));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_1, 4));
        CHECK_EQUAL(0, target_1->get_backlink_count(2, *origin_2, 0));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_1, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 4));
    }

    // Check that when the last column is removed from a target column, then its
    // size (number of rows) jumps to zero, and all links to it a removed or
    // nullified.
    {
        WriteTransaction wt(sg_1);
        TableRef target_1_w = wt.get_table("target_1");
        target_1_w->remove_column(0);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        check(test_context, sg_1, rt);
        CHECK_EQUAL(4, rt.get_group().size());
        ConstTableRef origin_1 = rt.get_table("origin_1");
        ConstTableRef origin_2 = rt.get_table("origin_2");
        ConstTableRef target_1 = rt.get_table("target_1");
        ConstTableRef target_2 = rt.get_table("target_2");
        CHECK_EQUAL(0, target_1->get_column_count());
        CHECK_EQUAL(1, target_2->get_column_count());
        CHECK_EQUAL(type_Link, target_2->get_column_type(0));
        CHECK_EQUAL(3, origin_1->size());
        CHECK_EQUAL(3, origin_2->size());
        CHECK_EQUAL(0, target_1->size());
        CHECK_EQUAL(3, target_2->size());
        CHECK(origin_1->is_null_link(0, 0));
        CHECK(origin_1->is_null_link(0, 1));
        CHECK(origin_1->is_null_link(0, 2));
        CHECK_EQUAL(1, origin_1->get_link(2, 0));
        CHECK(origin_1->is_null_link(2, 1));
        CHECK_EQUAL(0, origin_1->get_link(2, 2));
        CHECK(origin_2->is_null_link(0, 0));
        CHECK(origin_2->is_null_link(0, 1));
        CHECK(origin_2->is_null_link(0, 2));
        CHECK_EQUAL(0, origin_2->get_link(4, 0));
        CHECK_EQUAL(1, origin_2->get_link(4, 1));
        CHECK(origin_2->is_null_link(4, 2));
        ConstLinkViewRef link_list_1_0 = origin_1->get_linklist(4, 0);
        ConstLinkViewRef link_list_1_1 = origin_1->get_linklist(4, 1);
        ConstLinkViewRef link_list_1_2 = origin_1->get_linklist(4, 2);
        ConstLinkViewRef link_list_2_0 = origin_2->get_linklist(2, 0);
        ConstLinkViewRef link_list_2_1 = origin_2->get_linklist(2, 1);
        ConstLinkViewRef link_list_2_2 = origin_2->get_linklist(2, 2);
        CHECK_EQUAL(0, link_list_1_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_1_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_1_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_2_0->get_origin_row_index());
        CHECK_EQUAL(1, link_list_2_1->get_origin_row_index());
        CHECK_EQUAL(2, link_list_2_2->get_origin_row_index());
        CHECK_EQUAL(0, link_list_1_0->size());
        CHECK_EQUAL(0, link_list_1_1->size());
        CHECK_EQUAL(0, link_list_1_2->size());
        CHECK_EQUAL(2, link_list_2_0->size());
        CHECK_EQUAL(1, link_list_2_0->get(0).get_index());
        CHECK_EQUAL(1, link_list_2_0->get(1).get_index());
        CHECK_EQUAL(0, link_list_2_1->size());
        CHECK_EQUAL(1, link_list_2_2->size());
        CHECK_EQUAL(0, link_list_2_2->get(0).get_index());
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_1, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(0, *origin_2, 4));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_1, 2));
        CHECK_EQUAL(2, target_2->get_backlink_count(1, *origin_2, 2));
        CHECK_EQUAL(1, target_2->get_backlink_count(1, *origin_2, 4));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_1, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 2));
        CHECK_EQUAL(0, target_2->get_backlink_count(2, *origin_2, 4));
    }
}


TEST(Replication_CascadeRemove_ColumnLink)
{
    SHARED_GROUP_TEST_PATH(path_1);
    SHARED_GROUP_TEST_PATH(path_2);

    util::Logger& replay_logger = test_context.logger;

    SharedGroup sg(path_1);
    MyTrivialReplication repl(path_2);
    SharedGroup sg_w(repl);

    {
        WriteTransaction wt(sg_w);
        Table& origin = *wt.add_table("origin");
        Table& target = *wt.add_table("target");
        origin.add_column_link(type_Link, "o_1", target, link_Strong);
        target.add_column(type_Int, "t_1");
        wt.commit();
    }

    // perform_change expects sg to be in a read transaction
    sg.begin_read();

    ConstTableRef target;
    ConstRow target_row_0, target_row_1;

    auto perform_change = [&](std::function<void(Table&)> func) {
        // Ensure there are two rows in each table, with each row in `origin`
        // pointing to the corresponding row in `target`
        {
            WriteTransaction wt(sg_w);
            Table& origin_w = *wt.get_table("origin");
            Table& target_w = *wt.get_table("target");

            origin_w.clear();
            target_w.clear();
            origin_w.add_empty_row(2);
            target_w.add_empty_row(2);
            origin_w[0].set_link(0, 0);
            origin_w[1].set_link(0, 1);

            wt.commit();
        }

        // Perform the modification
        {
            WriteTransaction wt(sg_w);
            func(*wt.get_table("origin"));
            wt.commit();
        }

        // Apply the changes to sg via replication
        sg.end_read();
        repl.replay_transacts(sg, replay_logger);
        const Group& group = sg.begin_read();
        group.verify();

        target = group.get_table("target");
        if (target->size() > 0)
            target_row_0 = target->get(0);
        if (target->size() > 1)
            target_row_1 = target->get(1);
        // Leave `group` and the target accessors in a state which can be tested
        // with the changes applied
    };

    // Break link by nullifying
    perform_change([](Table& origin) { origin[1].nullify_link(0); });
    CHECK(target_row_0 && !target_row_1);
    CHECK_EQUAL(target->size(), 1);

    // Break link by reassign
    perform_change([](Table& origin) { origin[1].set_link(0, 0); });
    CHECK(target_row_0 && !target_row_1);
    CHECK_EQUAL(target->size(), 1);

    // Avoid breaking link by reassigning self
    perform_change([](Table& origin) { origin[1].set_link(0, 1); });
    // Should not delete anything
    CHECK(target_row_0 && target_row_1);
    CHECK_EQUAL(target->size(), 2);

    // Break link by explicit row removal
    perform_change([](Table& origin) { origin[1].move_last_over(); });
    CHECK(target_row_0 && !target_row_1);
    CHECK_EQUAL(target->size(), 1);

    // Break link by clearing table
    perform_change([](Table& origin) { origin.clear(); });
    CHECK(!target_row_0 && !target_row_1);
    CHECK_EQUAL(target->size(), 0);
}


TEST(Replication_LinkListSelfLinkNullification)
{
    SHARED_GROUP_TEST_PATH(path_1);
    SHARED_GROUP_TEST_PATH(path_2);

    MyTrivialReplication repl(path_1);
    SharedGroup sg_1(repl);
    SharedGroup sg_2(path_2);

    util::Logger& replay_logger = test_context.logger;

    {
        WriteTransaction wt(sg_1);
        TableRef t = wt.add_table("t");
        t->add_column_link(type_LinkList, "l", *t);
        t->add_empty_row(2);
        LinkViewRef ll = t->get_linklist(0, 1);
        ll->add(1);
        ll->add(1);
        ll->add(0);
        LinkViewRef ll2 = t->get_linklist(0, 0);
        ll2->add(0);
        ll2->add(1);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);

    {
        WriteTransaction wt(sg_1);
        TableRef t = wt.get_table("t");
        t->move_last_over(0);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    ReadTransaction rt_2{sg_2};
    check(test_context, sg_1, rt_2);
}


TEST(LangBindHelper_AdvanceReadTransact_CascadeRemove_ColumnLinkList)
{
    SHARED_GROUP_TEST_PATH(path_1);
    SHARED_GROUP_TEST_PATH(path_2);

    util::Logger& replay_logger = test_context.logger;

    SharedGroup sg(path_1);
    MyTrivialReplication repl(path_2);
    SharedGroup sg_w(repl);

    {
        WriteTransaction wt(sg_w);
        Table& origin = *wt.add_table("origin");
        Table& target = *wt.add_table("target");
        origin.add_column_link(type_LinkList, "o_1", target, link_Strong);
        target.add_column(type_Int, "t_1");
        wt.commit();
    }

    // perform_change expects sg to be in a read transaction
    sg.begin_read();

    ConstTableRef target;
    ConstRow target_row_0, target_row_1;

    auto perform_change = [&](std::function<void(Table&)> func) {
        // Ensure there are two rows in each table, with each row in `origin`
        // pointing to the corresponding row in `target`
        {
            WriteTransaction wt(sg_w);
            Table& origin_w = *wt.get_table("origin");
            Table& target_w = *wt.get_table("target");

            origin_w.clear();
            target_w.clear();
            origin_w.add_empty_row(2);
            target_w.add_empty_row(2);
            origin_w[0].get_linklist(0)->add(0);
            origin_w[1].get_linklist(0)->add(0);
            origin_w[1].get_linklist(0)->add(1);

            wt.commit();
        }

        // Perform the modification
        {
            WriteTransaction wt(sg_w);
            func(*wt.get_table("origin"));
            wt.commit();
        }

        // Apply the changes to sg via replication
        sg.end_read();
        repl.replay_transacts(sg, replay_logger);
        const Group& group = sg.begin_read();
        group.verify();

        target = group.get_table("target");
        if (target->size() > 0)
            target_row_0 = target->get(0);
        if (target->size() > 1)
            target_row_1 = target->get(1);
        // Leave `group` and the target accessors in a state which can be tested
        // with the changes applied
    };

    // Break link by clearing list
    perform_change([](Table& origin) { origin[1].get_linklist(0)->clear(); });
    CHECK(target_row_0 && !target_row_1);
    CHECK_EQUAL(target->size(), 1);

    // Break link by removal from list
    perform_change([](Table& origin) { origin[1].get_linklist(0)->remove(1); });
    CHECK(target_row_0 && !target_row_1);
    CHECK_EQUAL(target->size(), 1);

    // Break link by reassign
    perform_change([](Table& origin) { origin[1].get_linklist(0)->set(1, 0); });
    CHECK(target_row_0 && !target_row_1);
    CHECK_EQUAL(target->size(), 1);

    // Avoid breaking link by reassigning self
    perform_change([](Table& origin) { origin[1].get_linklist(0)->set(1, 1); });
    // Should not delete anything
    CHECK(target_row_0 && target_row_1);
    CHECK_EQUAL(target->size(), 2);

    // Break link by explicit row removal
    perform_change([](Table& origin) { origin[1].move_last_over(); });
    CHECK(target_row_0 && !target_row_1);
    CHECK_EQUAL(target->size(), 1);

    // Break link by clearing table
    perform_change([](Table& origin) { origin.clear(); });
    CHECK(!target_row_0 && !target_row_1);
    CHECK_EQUAL(target->size(), 0);
}


TEST(Replication_NullStrings)
{
    SHARED_GROUP_TEST_PATH(path_1);
    SHARED_GROUP_TEST_PATH(path_2);

    util::Logger& replay_logger = test_context.logger;

    MyTrivialReplication repl(path_1);
    SharedGroup sg_1(repl);
    SharedGroup sg_2(path_2);

    {
        WriteTransaction wt(sg_1);
        TableRef table1 = wt.add_table("table");
        table1->add_column(type_String, "c1", true);
        table1->add_column(type_Binary, "b1", true);
        table1->add_empty_row(3); // default value is null

        table1->set_string(0, 1, StringData("")); // empty string
        table1->set_string(0, 2, realm::null());  // null

        table1->set_binary(1, 1, BinaryData("")); // empty string
        table1->set_binary(1, 2, BinaryData());   // null

        CHECK(table1->get_string(0, 0).is_null());
        CHECK(!table1->get_string(0, 1).is_null());
        CHECK(table1->get_string(0, 2).is_null());

        CHECK(table1->get_binary(1, 0).is_null());
        CHECK(!table1->get_binary(1, 1).is_null());
        CHECK(table1->get_binary(1, 2).is_null());

        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        ConstTableRef table2 = rt.get_table("table");

        CHECK(table2->get_string(0, 0).is_null());
        CHECK(!table2->get_string(0, 1).is_null());
        CHECK(table2->get_string(0, 2).is_null());

        CHECK(table2->get_binary(1, 0).is_null());
        CHECK(!table2->get_binary(1, 1).is_null());
        CHECK(table2->get_binary(1, 2).is_null());
    }
}

TEST(Replication_NullInteger)
{
    SHARED_GROUP_TEST_PATH(path_1);
    SHARED_GROUP_TEST_PATH(path_2);

    util::Logger& replay_logger = test_context.logger;

    MyTrivialReplication repl(path_1);
    SharedGroup sg_1(repl);
    SharedGroup sg_2(path_2);

    {
        WriteTransaction wt(sg_1);
        TableRef table1 = wt.add_table("table");
        table1->add_column(type_Int, "c1", true);
        table1->add_empty_row(3); // default value is null

        table1->set_int(0, 1, 0);
        table1->set_null(0, 2);

        CHECK(table1->is_null(0, 0));
        CHECK(!table1->is_null(0, 1));
        CHECK(table1->is_null(0, 2));

        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        ConstTableRef table2 = rt.get_table("table");

        CHECK(table2->is_null(0, 0));
        CHECK(!table2->is_null(0, 1));
        CHECK(table2->is_null(0, 2));
    }
}


TEST(Replication_SetUnique)
{
    SHARED_GROUP_TEST_PATH(path_1);
    SHARED_GROUP_TEST_PATH(path_2);

    util::Logger& replay_logger = test_context.logger;

    MyTrivialReplication repl(path_1);
    SharedGroup sg_1(repl);
    SharedGroup sg_2(path_2);

    {
        WriteTransaction wt(sg_1);
        TableRef table1 = wt.add_table("table");
        table1->add_column(type_Int, "c1");
        table1->add_column(type_String, "c2");
        table1->add_column(type_Int, "c3", true);
        table1->add_column(type_String, "c4", true);
        table1->add_search_index(0);
        table1->add_search_index(1);
        table1->add_search_index(2);
        table1->add_search_index(3);
        table1->add_empty_row(2);
        table1->set_int_unique(0, 0, 123);
        table1->set_string_unique(1, 0, "Hello, World!");
        // This will delete row 0! It is a bit counter intuative but this
        // is because we expect that SetUnique is called before filling in
        // other columns with data.
        table1->set_null_unique(2, 0);
        CHECK_EQUAL(table1->size(), 1);
        table1->set_string_unique(3, 0, "Hello, World!");
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        ConstTableRef table2 = rt.get_table("table");

        CHECK_EQUAL(table2->get_int(0, 0), 0);
        CHECK_EQUAL(table2->get_string(1, 0), "");
        CHECK(table2->is_null(2, 0));
        CHECK_EQUAL(table2->get_string(3, 0), "Hello, World!");
    }
}


TEST(Replication_AddRowWithKey)
{
    SHARED_GROUP_TEST_PATH(path_1);
    SHARED_GROUP_TEST_PATH(path_2);

    util::Logger& replay_logger = test_context.logger;

    MyTrivialReplication repl(path_1);
    SharedGroup sg_1(repl);
    SharedGroup sg_2(path_2);

    {
        WriteTransaction wt(sg_1);
        TableRef table1 = wt.add_table("table");
        table1->add_column(type_Int, "c1");
        table1->add_search_index(0);
        table1->add_row_with_key(0, 123);
        table1->add_row_with_key(0, 456);
        CHECK_EQUAL(table1->size(), 2);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        ConstTableRef table2 = rt.get_table("table");

        CHECK_EQUAL(table2->find_first_int(0, 123), 0);
        CHECK_EQUAL(table2->find_first_int(0, 456), 1);
    }
}


TEST(Replication_RenameGroupLevelTable_MoveGroupLevelTable_RenameColumn_MoveColumn)
{
    SHARED_GROUP_TEST_PATH(path_1);
    SHARED_GROUP_TEST_PATH(path_2);

    util::Logger& replay_logger = test_context.logger;

    MyTrivialReplication repl(path_1);
    SharedGroup sg_1(repl);
    SharedGroup sg_2(path_2);

    {
        WriteTransaction wt(sg_1);
        TableRef table1 = wt.add_table("foo");
        table1->add_column(type_Int, "a");
        table1->add_column(type_Int, "c");
        TableRef table2 = wt.add_table("foo2");
        wt.commit();
    }
    {
        WriteTransaction wt(sg_1);
        wt.get_group().rename_table("foo", "bar");
        auto bar = wt.get_table("bar");
        bar->rename_column(0, "b");
        _impl::TableFriend::move_column(*bar->get_descriptor(), 1, 0);
        wt.get_group().move_table(1, 0);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        ConstTableRef foo = rt.get_table("foo");
        CHECK(!foo);
        ConstTableRef bar = rt.get_table("bar");
        CHECK(bar);
        CHECK_EQUAL(1, bar->get_index_in_group());
        CHECK_EQUAL(1, bar->get_column_index("b"));
    }
}


TEST(Replication_MergeRows)
{
    // Test that MergeRows has the same effect whether called directly
    // or applied via TransactLogApplier.

    SHARED_GROUP_TEST_PATH(path_1);
    SHARED_GROUP_TEST_PATH(path_2);

    util::Logger& replay_logger = test_context.logger;

    MyTrivialReplication repl(path_1);
    SharedGroup sg_1(repl);
    SharedGroup sg_2(path_2);

    {
        WriteTransaction wt(sg_1);
        TableRef t0 = wt.add_table("t0");
        TableRef t1 = wt.add_table("t1");
        t0->add_column(type_Int, "i");
        t1->add_column_link(type_Link, "l", *t0);
        t0->add_empty_row(2);
        t1->add_empty_row(2);
        t1->set_link(0, 0, 0);
        t0->merge_rows(0, 1);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt1(sg_1);
        ReadTransaction rt2(sg_2);

        auto t0_1 = rt1.get_table("t0");
        auto t1_1 = rt1.get_table("t1");
        auto t0_2 = rt2.get_table("t0");
        auto t1_2 = rt2.get_table("t1");

        CHECK_EQUAL(t1_1->get_link(0, 0), 1);
        CHECK_EQUAL(t1_2->get_link(0, 0), 1);
    }
}


TEST(Replication_LinkListNullifyThroughTableView)
{
    SHARED_GROUP_TEST_PATH(path_1);
    SHARED_GROUP_TEST_PATH(path_2);

    util::Logger& replay_logger = test_context.logger;

    MyTrivialReplication repl(path_1);
    SharedGroup sg_1(repl);
    SharedGroup sg_2(path_2);

    {
        WriteTransaction wt(sg_1);
        TableRef t0 = wt.add_table("t0");
        TableRef t1 = wt.add_table("t1");
        t0->add_column_link(type_LinkList, "l", *t1);
        t1->add_column(type_Int, "i");
        t1->add_empty_row();
        t0->add_empty_row();
        t0->get_linklist(0, 0)->add(0);

        // Create a TableView for the table and remove the rows through that.
        auto tv = t1->where().find_all();
        tv.clear(RemoveMode::unordered);

        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt1(sg_1);
        ReadTransaction rt2(sg_2);

        CHECK(rt1.get_group() == rt2.get_group());
        CHECK_EQUAL(rt1.get_table(0)->size(), 1);
        CHECK_EQUAL(rt1.get_table(1)->size(), 0);
        CHECK_EQUAL(rt1.get_table(0)->get_linklist(0, 0)->size(), 0);
    }
}


TEST(Replication_Substrings)
{
    SHARED_GROUP_TEST_PATH(path_1);
    SHARED_GROUP_TEST_PATH(path_2);

    util::Logger& replay_logger = test_context.logger;

    MyTrivialReplication repl(path_1);
    SharedGroup sg_1(repl);
    SharedGroup sg_2(path_2);

    {
        WriteTransaction wt(sg_1);
        TableRef table = wt.add_table("table");
        table->add_column(type_String, "string");
        table->add_empty_row();
        table->set_string(0, 0, "Hello, World!");
        wt.commit();
    }
    {
        WriteTransaction wt(sg_1);
        TableRef table = wt.get_table("table");
        table->remove_substring(0, 0, 0, 6);
        table->insert_substring(0, 0, 0, "Goodbye, Cruel");
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        auto table = rt.get_table("table");
        CHECK_EQUAL("Goodbye, Cruel World!", table->get_string(0, 0));
    }
}


TEST(Replication_MoveSelectedLinkView)
{
    // 1st: Create table with two rows
    // 2nd: Select link list via 2nd row
    // 3rd: Delete first row by move last over (which moves the row of the selected link list)
    // 4th: Modify the selected link list.
    // 5th: Replay changeset on different Realm

    SHARED_GROUP_TEST_PATH(path_1);
    SHARED_GROUP_TEST_PATH(path_2);

    util::Logger& replay_logger = test_context.logger;

    MyTrivialReplication repl(path_1);
    SharedGroup sg_1(repl);
    SharedGroup sg_2(path_2);

    {
        WriteTransaction wt(sg_1);
        TableRef origin = wt.add_table("origin");
        TableRef target = wt.add_table("target");
        origin->add_column_link(type_LinkList, "", *target);
        target->add_column(type_Int, "");
        origin->add_empty_row(2);
        target->add_empty_row(2);
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        rt.get_group().verify();
    }

    {
        WriteTransaction wt(sg_1);
        TableRef origin = wt.get_table("origin");
        LinkViewRef link_list = origin->get_linklist(0, 1);
        link_list->add(0);         // Select the link list of the 2nd row
        origin->move_last_over(0); // Move that link list
        link_list->add(1);         // Now modify it again
        wt.commit();
    }
    repl.replay_transacts(sg_2, replay_logger);
    {
        ReadTransaction rt(sg_2);
        rt.get_group().verify();
        ConstTableRef origin = rt.get_table("origin");
        ConstLinkViewRef link_list = origin->get_linklist(0, 0);
        CHECK_EQUAL(2, link_list->size());
    }

    // FIXME: Redo the test with all other table-level operations that move the
    // link list to a new row or column index.
}


} // anonymous namespace

#endif // TEST_REPLICATION
