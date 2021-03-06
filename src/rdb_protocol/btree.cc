// Copyright 2010-2012 RethinkDB, all rights reserved.
#include <string>
#include <vector>

#include "errors.hpp"
#include <boost/shared_ptr.hpp>
#include <boost/variant.hpp>

#include "btree/backfill.hpp"
#include "btree/depth_first_traversal.hpp"
#include "btree/erase_range.hpp"
#include "btree/get_distribution.hpp"
#include "btree/operations.hpp"
#include "btree/parallel_traversal.hpp"
#include "buffer_cache/blob.hpp"
#include "containers/archive/buffer_group_stream.hpp"
#include "containers/archive/vector_stream.hpp"
#include "containers/scoped.hpp"
#include "rdb_protocol/btree.hpp"
#include "rdb_protocol/transform_visitors.hpp"

typedef std::list<boost::shared_ptr<scoped_cJSON_t> > json_list_t;
typedef std::list<std::pair<store_key_t, boost::shared_ptr<scoped_cJSON_t> > > keyed_json_list_t;

#define MAX_RDB_VALUE_SIZE MAX_IN_NODE_VALUE_SIZE

struct rdb_value_t {
    char contents[];

public:
    int inline_size(block_size_t bs) const {
        return blob::ref_size(bs, contents, blob::btree_maxreflen);
    }

    int64_t value_size() const {
        return blob::value_size(contents, blob::btree_maxreflen);
    }

    const char *value_ref() const {
        return contents;
    }

    char *value_ref() {
        return contents;
    }
};

value_sizer_t<rdb_value_t>::value_sizer_t(block_size_t bs) : block_size_(bs) { }

const rdb_value_t *value_sizer_t<rdb_value_t>::as_rdb(const void *p) {
    return reinterpret_cast<const rdb_value_t *>(p);
}

int value_sizer_t<rdb_value_t>::size(const void *value) const {
    return as_rdb(value)->inline_size(block_size_);
}

bool value_sizer_t<rdb_value_t>::fits(const void *value, int length_available) const {
    return btree_value_fits(block_size_, length_available, as_rdb(value));
}

bool value_sizer_t<rdb_value_t>::deep_fsck(block_getter_t *getter, const void *value, int length_available, std::string *msg_out) const {
    if (!fits(value, length_available)) {
        *msg_out = "value does not fit in length_available";
        return false;
    }

    return blob::deep_fsck(getter, block_size_, as_rdb(value)->value_ref(), blob::btree_maxreflen, msg_out);
}

int value_sizer_t<rdb_value_t>::max_possible_size() const {
    return blob::btree_maxreflen;
}

block_magic_t value_sizer_t<rdb_value_t>::leaf_magic() {
    block_magic_t magic = { { 'r', 'd', 'b', 'l' } };
    return magic;
}

block_magic_t value_sizer_t<rdb_value_t>::btree_leaf_magic() const {
    return leaf_magic();
}

block_size_t value_sizer_t<rdb_value_t>::block_size() const { return block_size_; }

boost::shared_ptr<scoped_cJSON_t> get_data(const rdb_value_t *value,
                                           transaction_t *txn) {
    blob_t blob(const_cast<rdb_value_t *>(value)->value_ref(), blob::btree_maxreflen);

    boost::shared_ptr<scoped_cJSON_t> data;

    blob_acq_t acq_group;
    buffer_group_t buffer_group;
    blob.expose_all(txn, rwi_read, &buffer_group, &acq_group);
    buffer_group_read_stream_t read_stream(const_view(&buffer_group));
    int res = deserialize(&read_stream, &data);
    guarantee_err(res == 0, "corruption detected... this should probably be an exception\n");

    return data;
}

bool btree_value_fits(block_size_t bs, int data_length, const rdb_value_t *value) {
    return blob::ref_fits(bs, data_length, value->value_ref(), blob::btree_maxreflen);
}

void rdb_get(const store_key_t &store_key, btree_slice_t *slice, transaction_t *txn, superblock_t *superblock, point_read_response_t *response) {
    keyvalue_location_t<rdb_value_t> kv_location;
    find_keyvalue_location_for_read(txn, superblock, store_key.btree_key(), &kv_location, slice->root_eviction_priority, &slice->stats);

    if (!kv_location.value.has()) {
        response->data.reset(new scoped_cJSON_t(cJSON_CreateNull()));
    } else {
        response->data = get_data(kv_location.value.get(), txn);
    }
}

void kv_location_delete(keyvalue_location_t<rdb_value_t> *kv_location, const store_key_t &key,
                        btree_slice_t *slice, repli_timestamp_t timestamp, transaction_t *txn) {
    guarantee(kv_location->value.has());
    blob_t blob(kv_location->value->value_ref(), blob::btree_maxreflen);
    blob.clear(txn);
    kv_location->value.reset();
    null_key_modification_callback_t<rdb_value_t> null_cb;
    apply_keyvalue_change(txn, kv_location, key.btree_key(), timestamp, false, &null_cb, &slice->root_eviction_priority);
}

void kv_location_set(keyvalue_location_t<rdb_value_t> *kv_location, const store_key_t &key,
                     boost::shared_ptr<scoped_cJSON_t> data,
                     btree_slice_t *slice, repli_timestamp_t timestamp, transaction_t *txn) {

    scoped_malloc_t<rdb_value_t> new_value(MAX_RDB_VALUE_SIZE);
    bzero(new_value.get(), MAX_RDB_VALUE_SIZE);

    //TODO unnecessary copies they must go away.
    write_message_t wm;
    wm << data;
    vector_stream_t stream;
    int res = send_write_message(&stream, &wm);
    guarantee_err(res == 0, "Serialization for json data failed... this shouldn't happen.\n");

    blob_t blob(new_value->value_ref(), blob::btree_maxreflen);

    //TODO more copies, good lord
    blob.append_region(txn, stream.vector().size());
    std::string sered_data(stream.vector().begin(), stream.vector().end());
    blob.write_from_string(sered_data, txn, 0);

    // Actually update the leaf, if needed.
    kv_location->value.reinterpret_swap(new_value);
    null_key_modification_callback_t<rdb_value_t> null_cb;
    apply_keyvalue_change(txn, kv_location, key.btree_key(), timestamp, false, &null_cb, &slice->root_eviction_priority);
    //                                                                  ^^^^^ That means the key isn't expired.
}

// QL2 This implements UPDATE, REPLACE, and part of DELETE and INSERT (each is
// just a different function passed to this function).
void rdb_replace(btree_slice_t *slice,
                 repli_timestamp_t timestamp,
                 transaction_t *txn,
                 superblock_t *superblock,
                 const std::string &primary_key,
                 const store_key_t &key,
                 ql::map_wire_func_t *f,
                 ql::env_t *ql_env,
                 Datum *response_out,
                 rdb_modification_report_t *mod_report_out) THROWS_NOTHING {
    const ql::datum_t *num_1 = ql_env->add_ptr(new ql::datum_t(1.0));
    ql::datum_t *resp = ql_env->add_ptr(new ql::datum_t(ql::datum_t::R_OBJECT));
    try {
        keyvalue_location_t<rdb_value_t> kv_location;
        find_keyvalue_location_for_write(
            txn, superblock, key.btree_key(), &kv_location,
            &slice->root_eviction_priority, &slice->stats);

        bool started_empty, ended_empty;
        const ql::datum_t *old_val;
        if (!kv_location.value.has()) {
            // If there's no entry with this key, pass NULL to the function.
            started_empty = true;
            old_val = ql_env->add_ptr(new ql::datum_t(ql::datum_t::R_NULL));
        } else {
            // Otherwise pass the entry with this key to the function.
            started_empty = false;
            boost::shared_ptr<scoped_cJSON_t> old_val_json =
                get_data(kv_location.value.get(), txn);
            guarantee(old_val_json->GetObjectItem(primary_key.c_str()));
            old_val = ql_env->add_ptr(new ql::datum_t(old_val_json, ql_env));
        }
        guarantee(old_val);

        const ql::datum_t *new_val = f->compile(ql_env)->call(old_val)->as_datum();
        if (new_val->get_type() == ql::datum_t::R_NULL) {
            ended_empty = true;
        } else if (new_val->get_type() == ql::datum_t::R_OBJECT) {
            ended_empty = false;
            rcheck_target(
                new_val, new_val->el(primary_key, ql::NOTHROW),
                strprintf("Inserted object must have primary key `%s`:\n%s",
                          primary_key.c_str(), new_val->print().c_str()));
        } else {
            rfail_target(new_val, "Inserted value must be an OBJECT (got %s):\n%s",
                         new_val->get_type_name(), new_val->print().c_str());
        }

        // We use `conflict` below to store whether or not there was a key
        // conflict when constructing the stats object.  It defaults to `true`
        // so that we fail an assertion if we never update the stats object.
        bool conflict = true;
        // Figure out what operation we're doing (based on started_empty,
        // ended_empty, and the result of the function call) and then do it.
        if (started_empty) {
            if (ended_empty) {
                conflict = resp->add("skipped", num_1);
            } else {
                conflict = resp->add("inserted", num_1);
                r_sanity_check(new_val->el(primary_key, ql::NOTHROW));
                boost::shared_ptr<scoped_cJSON_t> new_val_as_json = new_val->as_json();
                kv_location_set(&kv_location, key, new_val_as_json,
                                slice, timestamp, txn);
                mod_report_out->added = new_val_as_json;
            }
        } else {
            if (ended_empty) {
                conflict = resp->add("deleted", num_1);
                kv_location_delete(&kv_location, key, slice, timestamp, txn);
                mod_report_out->deleted = old_val->as_json();
            } else {
                if (*old_val->el(primary_key) == *new_val->el(primary_key)) {
                    if (*old_val == *new_val) {
                        conflict = resp->add("unchanged", num_1);
                    } else {
                        conflict = resp->add("replaced", num_1);
                        r_sanity_check(new_val->el(primary_key, ql::NOTHROW));
                        boost::shared_ptr<scoped_cJSON_t> new_val_as_json
                            = new_val->as_json();
                        kv_location_set(&kv_location, key, new_val_as_json,
                                        slice, timestamp, txn);
                        mod_report_out->added = new_val_as_json;
                        mod_report_out->deleted = old_val->as_json();
                    }
                } else {
                    rfail_target(
                        new_val,
                        "Primary key `%s` cannot be changed (%s -> %s)",
                        primary_key.c_str(),
                        old_val->print().c_str(), new_val->print().c_str());
                }
            }
        }
        guarantee(!conflict); // message never added twice
    } catch (const ql::any_ql_exc_t &e) {
        std::string msg = e.what();
        bool b = resp->add("errors", num_1)
              || resp->add("first_error", ql_env->add_ptr(new ql::datum_t(msg)));
        guarantee(!b);
    }
    resp->write_to_protobuf(response_out);
}

void rdb_set(const store_key_t &key, boost::shared_ptr<scoped_cJSON_t> data, bool overwrite,
             btree_slice_t *slice, repli_timestamp_t timestamp,
             transaction_t *txn, superblock_t *superblock, point_write_response_t *response,
             rdb_modification_report_t *mod_report) {
    //block_size_t block_size = slice->cache()->get_block_size();
    keyvalue_location_t<rdb_value_t> kv_location;
    find_keyvalue_location_for_write(txn, superblock, key.btree_key(), &kv_location, &slice->root_eviction_priority, &slice->stats);
    bool had_value = kv_location.value.has();

    /* update the modification report */
    if (kv_location.value.has()) {
        mod_report->deleted = get_data(kv_location.value.get(), txn);
    }

    mod_report->added = data;

    if (overwrite || !had_value) {
        kv_location_set(&kv_location, key, data, slice, timestamp, txn);
    }
    response->result = (had_value ? DUPLICATE : STORED);
}

class agnostic_rdb_backfill_callback_t : public agnostic_backfill_callback_t {
public:
    agnostic_rdb_backfill_callback_t(rdb_backfill_callback_t *cb, const key_range_t &kr) : cb_(cb), kr_(kr) { }

    void on_delete_range(const key_range_t &range, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
        rassert(kr_.is_superset(range));
        cb_->on_delete_range(range, interruptor);
    }

    void on_deletion(const btree_key_t *key, repli_timestamp_t recency, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
        rassert(kr_.contains_key(key->contents, key->size));
        cb_->on_deletion(key, recency, interruptor);
    }

    void on_pair(transaction_t *txn, repli_timestamp_t recency, const btree_key_t *key, const void *val, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
        rassert(kr_.contains_key(key->contents, key->size));
        const rdb_value_t *value = static_cast<const rdb_value_t *>(val);

        rdb_protocol_details::backfill_atom_t atom;
        atom.key.assign(key->size, key->contents);
        atom.value = get_data(value, txn);
        atom.recency = recency;
        cb_->on_keyvalue(atom, interruptor);
    }

    void on_sindexes(const std::map<uuid_u, secondary_index_t> &sindexes, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
        cb_->on_sindexes(sindexes, interruptor);
    }

    rdb_backfill_callback_t *cb_;
    key_range_t kr_;
};

void rdb_backfill(btree_slice_t *slice, const key_range_t& key_range,
        repli_timestamp_t since_when, rdb_backfill_callback_t *callback,
        transaction_t *txn, superblock_t *superblock,
        buf_lock_t *sindex_block,
        parallel_traversal_progress_t *p, signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    agnostic_rdb_backfill_callback_t agnostic_cb(callback, key_range);
    value_sizer_t<rdb_value_t> sizer(slice->cache()->get_block_size());
    do_agnostic_btree_backfill(&sizer, slice, key_range, since_when, &agnostic_cb, txn, superblock, sindex_block, p, interruptor);
}

void rdb_delete(const store_key_t &key, btree_slice_t *slice,
                repli_timestamp_t timestamp, transaction_t *txn,
                superblock_t *superblock, point_delete_response_t *response,
                rdb_modification_report_t *mod_report_out) {
    keyvalue_location_t<rdb_value_t> kv_location;
    find_keyvalue_location_for_write(txn, superblock, key.btree_key(), &kv_location, &slice->root_eviction_priority, &slice->stats);
    bool exists = kv_location.value.has();

    /* Update the modification report. */
    if (exists) {
        mod_report_out->deleted = get_data(kv_location.value.get(), txn);
    }

    if (exists) kv_location_delete(&kv_location, key, slice, timestamp, txn);
    response->result = (exists ? DELETED : MISSING);
}

void rdb_value_deleter_t::delete_value(transaction_t *_txn, void *_value) {
        blob_t blob(static_cast<rdb_value_t *>(_value)->value_ref(), blob::btree_maxreflen);
        blob.clear(_txn);
}

void rdb_erase_range(btree_slice_t *slice, key_tester_t *tester,
                     bool left_key_supplied, const store_key_t& left_key_exclusive,
                     bool right_key_supplied, const store_key_t& right_key_inclusive,
                     transaction_t *txn, superblock_t *superblock) {

    value_sizer_t<rdb_value_t> rdb_sizer(slice->cache()->get_block_size());
    value_sizer_t<void> *sizer = &rdb_sizer;

    rdb_value_deleter_t deleter;

    btree_erase_range_generic(sizer, slice, tester, &deleter,
        left_key_supplied ? left_key_exclusive.btree_key() : NULL,
        right_key_supplied ? right_key_inclusive.btree_key() : NULL,
        txn, superblock);
}

void rdb_erase_range(btree_slice_t *slice, key_tester_t *tester,
                       const key_range_t &keys,
                       transaction_t *txn, superblock_t *superblock) {
    store_key_t left_exclusive(keys.left);
    store_key_t right_inclusive(keys.right.key);

    bool left_key_supplied = left_exclusive.decrement();
    bool right_key_supplied = !keys.right.unbounded;
    if (right_key_supplied) {
        right_inclusive.decrement();
    }
    rdb_erase_range(slice, tester, left_key_supplied, left_exclusive, right_key_supplied, right_inclusive, txn, superblock);
}

size_t estimate_rget_response_size(const boost::shared_ptr<scoped_cJSON_t> &/*json*/) {
    // TODO: don't be stupid, be a smarty, come and join the nazy
    // party (json size estimation will be much easier once we switch
    // to bson -- fuck it for now).
    return 250;
}

class rdb_rget_depth_first_traversal_callback_t : public depth_first_traversal_callback_t {
public:
    rdb_rget_depth_first_traversal_callback_t(
        transaction_t *txn,
        ql::env_t *_ql_env,
        const rdb_protocol_details::transform_t &_transform,
        boost::optional<rdb_protocol_details::terminal_t> _terminal,
        const key_range_t &range,
        rget_read_response_t *_response)
        : bad_init(false), transaction(txn), response(_response), cumulative_size(0),
          ql_env(_ql_env), transform(_transform), terminal(_terminal)
    {
        try {
            response->last_considered_key = range.left;

            if (terminal) {
                boost::apply_visitor(query_language::terminal_initializer_visitor_t(
                                         &response->result, ql_env,
                                         terminal->scopes, terminal->backtrace),
                                     terminal->variant);
            }
        } catch (const query_language::runtime_exc_t &e) {
            /* Evaluation threw so we're not going to be accepting any more requests. */
            response->result = e;
            bad_init = true;
        } catch (const ql::exc_t &e2) {
            /* Evaluation threw so we're not going to be accepting any more requests. */
            response->result = e2;
            bad_init = true;
        }
    }

    bool handle_pair(const btree_key_t* key, const void *value) {
        if (bad_init) return false;
        try {
            store_key_t store_key(key);
            if (response->last_considered_key < store_key) {
                response->last_considered_key = store_key;
            }

            const rdb_value_t *rdb_value = reinterpret_cast<const rdb_value_t *>(value);

            json_list_t data;
            data.push_back(get_data(rdb_value, transaction));

            //Apply transforms to the data
            {
                rdb_protocol_details::transform_t::iterator it;
                for (it = transform.begin(); it != transform.end(); ++it) {
                    json_list_t tmp;

                    for (json_list_t::iterator jt  = data.begin();
                         jt != data.end();
                         ++jt) {
                        boost::apply_visitor(query_language::transform_visitor_t(
                                                 *jt, &tmp, ql_env, it->scopes,
                                                 it->backtrace), it->variant);
                    }
                    data.clear();
                    data.splice(data.begin(), tmp);
                }
            }

            if (!terminal) {
                typedef rget_read_response_t::stream_t stream_t;
                stream_t *stream = boost::get<stream_t>(&response->result);
                guarantee(stream);
                for (json_list_t::iterator it =  data.begin();
                                           it != data.end();
                                           ++it) {
                    stream->push_back(std::make_pair(store_key_t(key), *it));
                    cumulative_size += estimate_rget_response_size(*it);
                }

                return cumulative_size < rget_max_chunk_size;
            } else {
                // We use garbage collection during the reduction step, since
                // most reductions throw away most of the allocate data.
                ql::env_gc_checkpoint_t egct(ql_env);
                int i = 0;
                json_list_t::iterator jt;
                for (jt = data.begin(); jt != data.end(); ++jt) {
                    boost::apply_visitor(query_language::terminal_visitor_t(
                                             *jt, ql_env, terminal->scopes,
                                             terminal->backtrace, &response->result),
                                         terminal->variant);
                    // A reduce returns a `wire_datum_t` and a gmr returns a
                    // `wire_datum_map_t`
                    if (ql::wire_datum_t *wd
                        = boost::get<ql::wire_datum_t>(&terminal->variant)) {
                        egct.maybe_gc(wd->get());
                    } else if (ql::wire_datum_map_t *wdm
                               = boost::get<ql::wire_datum_map_t>(
                                   &terminal->variant)) {
                        // TODO: this is a hack because GCing a `wire_datum_map_t` is
                        // expensive.  Need a better way to do this.
                        int rounds = 10000 DEBUG_ONLY(/ 5000);
                        if (!(++i % rounds)) egct.maybe_gc(wdm->to_arr(ql_env));
                    }

                }
                return true;
            }
        } catch (const query_language::runtime_exc_t &e) {
            /* Evaluation threw so we're not going to be accepting any more requests. */
            response->result = e;
            return false;
        } catch (const ql::exc_t &e2) {
            /* Evaluation threw so we're not going to be accepting any more requests. */
            response->result = e2;
            return false;
        }

    }
    bool bad_init;
    transaction_t *transaction;
    rget_read_response_t *response;
    size_t cumulative_size;
    ql::env_t *ql_env;
    rdb_protocol_details::transform_t transform;
    boost::optional<rdb_protocol_details::terminal_t> terminal;
};

void rdb_rget_slice(btree_slice_t *slice, const key_range_t &range,
                    transaction_t *txn, superblock_t *superblock,
                    ql::env_t *ql_env,
                    const rdb_protocol_details::transform_t &transform,
                    const boost::optional<rdb_protocol_details::terminal_t> &terminal,
                    rget_read_response_t *response) {
    rdb_rget_depth_first_traversal_callback_t callback(txn, ql_env, transform, terminal, range, response);
    btree_depth_first_traversal(slice, txn, superblock, range, &callback);

    if (callback.cumulative_size >= rget_max_chunk_size) {
        response->truncated = true;
    } else {
        response->truncated = false;
    }

    //TODO: change this whole file so that this isn't necessary.
    if (ql::wire_datum_t *d = boost::get<ql::wire_datum_t>(&response->result)) {
        d->finalize();
    } else if (ql::wire_datum_map_t *dm =
               boost::get<ql::wire_datum_map_t>(&response->result)) {
        dm->finalize();
    }
}

void rdb_distribution_get(btree_slice_t *slice, int max_depth, const store_key_t &left_key,
                          transaction_t *txn, superblock_t *superblock, distribution_read_response_t *response) {
    int64_t key_count_out;
    std::vector<store_key_t> key_splits;
    get_btree_key_distribution(slice, txn, superblock, max_depth, &key_count_out, &key_splits);

    int64_t keys_per_bucket;
    if (key_splits.size() == 0) {
        keys_per_bucket = key_count_out;
    } else  {
        keys_per_bucket = std::max<int64_t>(key_count_out / key_splits.size(), 1);
    }
    response->key_counts[left_key] = keys_per_bucket;

    for (std::vector<store_key_t>::iterator it  = key_splits.begin();
                                            it != key_splits.end();
                                            ++it) {
        response->key_counts[*it] = keys_per_bucket;
    }
}

namespace {
enum rdb_modification_has_value_t {
    HAS_VALUE,
    HAS_NO_VALUE
};

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(rdb_modification_has_value_t, int8_t, HAS_VALUE, HAS_NO_VALUE);
} //anonymous namespace

void rdb_modification_report_t::rdb_serialize(write_message_t &msg /* NOLINT */) const {
    msg << primary_key;
    if (!deleted.get()) {
        msg << HAS_NO_VALUE;
    } else {
        msg << HAS_VALUE;
        msg << deleted;
    }

    if (!added.get()) {
        msg << HAS_NO_VALUE;
    } else {
        msg << HAS_VALUE;
        msg << added;
    }
}

archive_result_t rdb_modification_report_t::rdb_deserialize(read_stream_t *s) {
    archive_result_t res;

    res = deserialize(s, &primary_key);
    if (res) { return res; }

    rdb_modification_has_value_t has_value;
    res = deserialize(s, &has_value);
    if (res) { return res; }

    if (has_value == HAS_VALUE) {
        res = deserialize(s, &deleted);
        if (res) { return res; }
    }

    res = deserialize(s, &has_value);
    if (res) { return res; }

    if (has_value == HAS_VALUE) {
        res = deserialize(s, &added);
        if (res) { return res; }
    }

    return ARCHIVE_SUCCESS;
}

typedef btree_store_t<rdb_protocol_t>::sindex_access_vector_t sindex_access_vector_t;

/* A target for pmap. Used below by rdb_update_sindexes. */
void rdb_update_single_sindex(
        const btree_store_t<rdb_protocol_t>::sindex_access_t *sindex,
        rdb_modification_report_t *modification,
        transaction_t *txn,
        auto_drainer_t::lock_t) {
    //Note if you get this error it's likely that you've passed in a default
    //constructed mod_report. Don't do that mod reports should always be passed
    //to a function as an output parameter before they're passed to this
    //function.
    guarantee(modification->primary_key.size() != 0);
    ql::map_wire_func_t mapping;
    vector_read_stream_t read_stream(&sindex->sindex.opaque_definition);
    int success = deserialize(&read_stream, &mapping);
    guarantee(success == ARCHIVE_SUCCESS, "Corrupted sindex description.");

    //TODO we just use a NULL environment here. People should not be able
    //to do anything that requires an environment like gets from other
    //tables etc. but we don't have a nice way to disallow those things so
    //for now we pass null and it will segfault if an illegal sindex
    //mapping is passed.
    cond_t non_interruptor;
    ql::env_t env(&non_interruptor);

    superblock_t *super_block = sindex->super_block.get();

    if (modification->deleted) {
        promise_t<superblock_t *> return_superblock_local;
        {

            const ql::datum_t *deleted = env.add_ptr(new ql::datum_t(modification->deleted, &env));

            const ql::datum_t *index =
                mapping.compile(&env)->call(deleted)->as_datum();

            store_key_t sindex_key(index->print_secondary(modification->primary_key));

            keyvalue_location_t<rdb_value_t> kv_location;

            find_keyvalue_location_for_write(txn, super_block,
                    sindex_key.btree_key(), &kv_location,
                    &sindex->btree->root_eviction_priority, &sindex->btree->stats,
                    &return_superblock_local);

            kv_location_delete(&kv_location, sindex_key,
                        sindex->btree, repli_timestamp_t::distant_past, txn);
            //The keyvalue location gets destroyed here.
        }
        super_block = return_superblock_local.wait();
    }

    if (modification->added) {
        const ql::datum_t *added = env.add_ptr(new ql::datum_t(modification->added, &env));

        const ql::datum_t *index =
            mapping.compile(&env)->call(added)->as_datum();

        store_key_t sindex_key(index->print_secondary(modification->primary_key));

        keyvalue_location_t<rdb_value_t> kv_location;

        promise_t<superblock_t *> dummy;
        find_keyvalue_location_for_write(txn, super_block,
                sindex_key.btree_key(), &kv_location,
                &sindex->btree->root_eviction_priority, &sindex->btree->stats,
                &dummy);

        kv_location_set(&kv_location, sindex_key,
                 modification->added, sindex->btree, repli_timestamp_t::distant_past, txn);
    }
}

void rdb_update_sindexes(const sindex_access_vector_t &sindexes,
        rdb_modification_report_t *modification,
        transaction_t *txn) {
    auto_drainer_t drainer;

    for (sindex_access_vector_t::const_iterator it  = sindexes.begin();
                                                it != sindexes.end();
                                                ++it) {
        coro_t::spawn_sometime(boost::bind(
                    &rdb_update_single_sindex, &*it,
                    modification, txn, auto_drainer_t::lock_t(&drainer)));
    }
}

class post_construct_traversal_helper_t : public btree_traversal_helper_t {
public:
    post_construct_traversal_helper_t(
            btree_store_t<rdb_protocol_t> *store,
            const std::set<uuid_u> &sindexes_to_post_construct,
            signal_t *interruptor
            )
        : store_(store),
          sindexes_to_post_construct_(sindexes_to_post_construct),
          interruptor_(interruptor)
    { }

    void process_a_leaf(transaction_t *txn, buf_lock_t *leaf_node_buf,
                        const btree_key_t *, const btree_key_t *,
                        signal_t *, int *) THROWS_ONLY(interrupted_exc_t) {
        write_token_pair_t token_pair;
        store_->new_write_token_pair(&token_pair);

        scoped_ptr_t<transaction_t> wtxn;
        btree_store_t<rdb_protocol_t>::sindex_access_vector_t sindexes;
        {
            scoped_ptr_t<real_superblock_t> superblock;

            store_->acquire_superblock_for_write(
                rwi_write,
                repli_timestamp_t::distant_past,
                2,
                &token_pair.main_write_token,
                &wtxn,
                &superblock,
                interruptor_);

            scoped_ptr_t<buf_lock_t> sindex_block;
            store_->acquire_sindex_block_for_write(
                &token_pair,
                wtxn.get(),
                &sindex_block,
                superblock->get_sindex_block_id(),
                interruptor_);

            store_->acquire_sindex_superblocks_for_write(
                    sindexes_to_post_construct_,
                    sindex_block.get(),
                    wtxn.get(),
                    &sindexes);
        }

        const leaf_node_t *leaf_node = reinterpret_cast<const leaf_node_t *>(leaf_node_buf->get_data_read());
        leaf::live_iter_t node_iter = leaf::iter_for_whole_leaf(leaf_node);

        const btree_key_t *key;
        while ((key = node_iter.get_key(leaf_node))) {
            /* Grab relevant values from the leaf node. */
            const void *value = node_iter.get_value(leaf_node);
            guarantee(key);
            node_iter.step(leaf_node);

            store_key_t pk(key);
            rdb_modification_report_t mod_report(pk);
            const rdb_value_t *rdb_value = reinterpret_cast<const rdb_value_t *>(value);
            mod_report.added = get_data(rdb_value, txn);

            rdb_update_sindexes(sindexes, &mod_report, wtxn.get());
        }
    }

    void postprocess_internal_node(buf_lock_t *) { }

    void filter_interesting_children(UNUSED transaction_t *txn, ranged_block_ids_t *ids_source, interesting_children_callback_t *cb) {
        for (int i = 0, e = ids_source->num_block_ids(); i < e; ++i) {
            cb->receive_interesting_child(i);
        }
        cb->no_more_interesting_children();
    }

    access_t btree_superblock_mode() { return rwi_read; }
    access_t btree_node_mode() { return rwi_read; }

    btree_store_t<rdb_protocol_t> *store_;
    const std::set<uuid_u> &sindexes_to_post_construct_;
    signal_t *interruptor_;
};

void post_construct_secondary_indexes(
        btree_store_t<rdb_protocol_t> *store,
        const std::set<uuid_u> &sindexes_to_post_construct,
        signal_t *interruptor)
    THROWS_ONLY(interrupted_exc_t) {
    post_construct_traversal_helper_t helper(store, 
            sindexes_to_post_construct, interruptor);

    object_buffer_t<fifo_enforcer_sink_t::exit_read_t> read_token;
    store->new_read_token(&read_token);

    scoped_ptr_t<transaction_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;

    store->acquire_superblock_for_read(
        rwi_read,
        &read_token,
        &txn,
        &superblock,
        interruptor,
        true /* USE_SNAPSHOT */);
    btree_parallel_traversal(txn.get(), superblock.get(), 
            store->btree.get(), &helper, interruptor);
}
