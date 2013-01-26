#include "NeuralTable.h"
/* !!!! A NOTE ON KEYS !!!!
 * Keys should be integer values passed from the erlang emulator, 
 * and should be generated by a hashing function. There is no easy 
 * way to hash an erlang term from a NIF, but ERTS is more than 
 * capable of doing so.
 *
 * Additionally, this workaround means that traditional collision 
 * handling mechanisms for hash tables will not work without 
 * special consideration. For instance, to compare keys as you
 * would by storing linked lists, you must retrieve the stored
 * tuple and call enif_compare or enif_is_identical on the key
 * elements of each tuple.
 */

table_set NeuralTable::tables;
atomic<bool> NeuralTable::running(true);

NeuralTable::NeuralTable(unsigned int kp) {
    for (int i = 0;  i < BUCKET_COUNT; ++i) {
        ErlNifEnv *env = enif_alloc_env();
        env_buckets[i] = env;
        locks[i] = enif_rwlock_create("neural_table");
        garbage_cans[i] = 0;
        reclaimable[i] = enif_make_list(env, 0);
    }

    start_gc();
    start_batch();

    key_pos = kp;
}

NeuralTable::~NeuralTable() {
    stop_batch();
    stop_gc();
    for (int i = 0; i < BUCKET_COUNT; ++i) {
        enif_rwlock_destroy(locks[i]);
        enif_free_env(env_buckets[i]);
    }
}

/* ================================================================
 * MakeTable
 * Allocates a new table, assuming a unique atom identifier. This
 * table is stored in a static container. All interactions with
 * the table must be performed through the static class API.
 */
ERL_NIF_TERM NeuralTable::MakeTable(ErlNifEnv *env, ERL_NIF_TERM name, ERL_NIF_TERM key_pos) {
    char *atom;
    string key;
    unsigned int len = 0,
                 pos = 0;

    // Allocate space for the name of the table
    enif_get_atom_length(env, name, &len, ERL_NIF_LATIN1);
    atom = (char*)enif_alloc(len + 1);

    // Fetch the value of the atom and store it in a string (because I can, that's why)
    enif_get_atom(env, name, atom, len + 1, ERL_NIF_LATIN1);
    key = atom;

    // Deallocate that space
    enif_free(atom);

    // Get the key position value
    enif_get_uint(env, key_pos, &pos);

    // Table already exists? Bad monkey!
    if (NeuralTable::tables.find(key) != NeuralTable::tables.end()) { return enif_make_badarg(env); }

    // All good. Make the table
    NeuralTable::tables[key] = new NeuralTable(pos);

    return enif_make_atom(env, "ok");
}

/* ================================================================
 * GetTable
 * Retrieves a handle to the table referenced by name, assuming
 * such a table exists. If not, throw badarg.
 */
NeuralTable* NeuralTable::GetTable(ErlNifEnv *env, ERL_NIF_TERM name) {
    char *atom = NULL;
    string key;
    unsigned len = 0;
    NeuralTable *ret = NULL;
    table_set::const_iterator it;

    // Allocate space for the table name
    enif_get_atom_length(env, name, &len, ERL_NIF_LATIN1);
    atom = (char*)enif_alloc(len + 1);

    // Copy the table name into a string
    enif_get_atom(env, name, atom, len + 1, ERL_NIF_LATIN1);
    key = atom;
    
    // Deallocate that space
    enif_free(atom);

    // Look for the table and return its pointer if found
    it = NeuralTable::tables.find(key);
    if (it != NeuralTable::tables.end()) { 
        ret = it->second;
    }

    return ret;
}

/* ================================================================
 * Insert
 * Inserts a tuple into the table with key. 
 */
ERL_NIF_TERM NeuralTable::Insert(ErlNifEnv *env, ERL_NIF_TERM table, ERL_NIF_TERM key, ERL_NIF_TERM object) {
    NeuralTable *tb;
    ERL_NIF_TERM ret, old;
    unsigned long int entry_key = 0;

    // Grab table or bail.
    tb = GetTable(env, table);
    if (tb == NULL) { 
        return enif_make_badarg(env); 
    }

    // Get key value.
    enif_get_ulong(env, key, &entry_key);

    // Lock the key.
    tb->rwlock(entry_key);

    // Attempt to lookup the value. If nonempty, increment
    // discarded term counter and return a copy of the
    // old value
    if (tb->find(entry_key, old)) {
        tb->reclaim(entry_key, old);
        ret = enif_make_tuple2(env, enif_make_atom(env, "ok"), enif_make_copy(env, old));
    } else {
        ret = enif_make_atom(env, "ok");
    }
    
    // Write that shit out
    tb->put(entry_key, object);

    // Oh, and unlock the key if you would.
    tb->rwunlock(entry_key);

    return ret;
}

/* ================================================================
 * InsertNew
 * Inserts a tuple into the table with key, assuming there is not
 * a value with key already. Returns true if there was no value
 * for key, or false if there was.
 */
ERL_NIF_TERM NeuralTable::InsertNew(ErlNifEnv *env, ERL_NIF_TERM table, ERL_NIF_TERM key, ERL_NIF_TERM object) {
    NeuralTable *tb;
    ERL_NIF_TERM ret, old;
    unsigned long int entry_key = 0;

    // Get the table or bail
    tb = GetTable(env, table);
    if (tb == NULL) {
        return enif_make_badarg(env);
    }

    // Get the key value
    enif_get_ulong(env, key, &entry_key);

    // Get write lock for the key
    tb->rwlock(entry_key);

    if (tb->find(entry_key, old)) {
        // Key was found. Return false and do not insert
        ret = enif_make_atom(env, "false");
    } else {
        // Key was not found. Return true and insert
        tb->put(entry_key, object);
        ret = enif_make_atom(env, "true");
    }

    // Release write lock for the key
    tb->rwunlock(entry_key);

    return ret;
}

/* ================================================================
 * Increment
 * Processes a list of update operations. Each operation specifies
 * a position in the stored tuple to update and an integer to add
 * to it.
 */
ERL_NIF_TERM NeuralTable::Increment(ErlNifEnv *env, ERL_NIF_TERM table, ERL_NIF_TERM key, ERL_NIF_TERM ops) {
    NeuralTable *tb;
    ERL_NIF_TERM ret, old;
    ERL_NIF_TERM it;
    unsigned long int entry_key = 0;

    // Get table handle or bail
    tb = GetTable(env, table);
    if (tb == NULL) {
        return enif_make_badarg(env);
    }

    // Get key value
    enif_get_ulong(env, key, &entry_key);

    // Acquire read/write lock for key
    tb->rwlock(entry_key);

    // Try to read the value as it is
    if (tb->find(entry_key, old)) {
        // Value exists
        ERL_NIF_TERM op_cell;
        const ERL_NIF_TERM *tb_tpl;
        const ERL_NIF_TERM *op_tpl;
        ERL_NIF_TERM *new_tpl;
        ErlNifEnv *bucket_env = tb->get_env(entry_key);
        unsigned long int   pos         = 0;
        long int            incr        = 0;
        unsigned int        ops_length  = 0;
        int                 op_arity    = 0,
                            tb_arity    = 0;

        // Expand tuple to work on elements
        enif_get_tuple(bucket_env, old, &tb_arity, &tb_tpl);

        // Allocate space for a copy the contents of the table
        // tuple and copy it in. All changes are to be made to
        // the copy of the tuple.
        new_tpl = (ERL_NIF_TERM*)enif_alloc(sizeof(ERL_NIF_TERM) * tb_arity);
        memcpy(new_tpl, tb_tpl, sizeof(ERL_NIF_TERM) * tb_arity);

        // Create empty list cell for return value.
        ret = enif_make_list(env, 0);

        // Set iterator to first cell of ops
        it = ops;
        while(!enif_is_empty_list(env, it)) {
            long int value = 0;
            enif_get_list_cell(env, it, &op_cell, &it);             // op_cell = hd(it), it = tl(it)
            enif_get_tuple(env, op_cell, &op_arity, &op_tpl);       // op_arity = tuple_size(op_cell), op_tpl = [TplPos1, TplPos2]
            enif_get_ulong(env, op_tpl[0], &pos);                   // pos = (uint64)op_tpl[0]
            enif_get_long(env, op_tpl[1], &incr);                   // incr = (uint64)op_tpl[1]

            // Is the operation trying to modify a nonexistant
            // position?
            if (pos <= 0 || pos > tb_arity) {
                ret = enif_make_badarg(env);
                goto bailout;
            }

            // Is the operation trying to add to a value that's
            // not a number?
            if (!enif_is_number(bucket_env, new_tpl[pos - 1])) {
                ret = enif_make_badarg(env);
                goto bailout;
            }

            // Update the value stored in the tuple.
            enif_get_long(env, new_tpl[pos - 1], &value);
            tb->reclaim(entry_key, new_tpl[pos - 1]);
            new_tpl[pos - 1] = enif_make_long(bucket_env, value + incr);

            // Copy the new value to the head of the return list
            ret = enif_make_list_cell(env, enif_make_copy(env, new_tpl[pos - 1]), ret);
        }

        tb->put(entry_key, enif_make_tuple_from_array(bucket_env, new_tpl, tb_arity));

        // Bailout allows cancelling the update opertion
        // in case something goes wrong. It must always 
        // come after tb->put and before enif_free and
        // rwunlock
bailout:
        enif_free(new_tpl);
    } else {
        ret = enif_make_badarg(env);
    }
    // Release the rwlock for entry_key
    tb->rwunlock(entry_key);

    return ret;
}

/* ================================================================
 * Unshift
 * Processes a list of update operations. Each update operation is
 * a tuple specifying the position of a list in the stored value to 
 * update and a list of values to append. Elements are shifted from
 * the input list to the stored list, so:
 *
 * unshift([a,b,c,d]) results in [d,c,b,a]
 */
ERL_NIF_TERM NeuralTable::Unshift(ErlNifEnv *env, ERL_NIF_TERM table, ERL_NIF_TERM key, ERL_NIF_TERM ops) {
    NeuralTable *tb;
    ERL_NIF_TERM ret, old, it;
    unsigned long int entry_key;
    ErlNifEnv *bucket_env;

    tb = GetTable(env, table);
    if (tb == NULL) {
        return enif_make_badarg(env);
    }

    enif_get_ulong(env, key, &entry_key);

    tb->rwlock(entry_key);
    bucket_env = tb->get_env(entry_key);
    if (tb->find(entry_key, old)) {
        const ERL_NIF_TERM  *old_tpl,
                            *op_tpl;
        ERL_NIF_TERM        *new_tpl;
        int tb_arity = 0,
            op_arity = 0;
        unsigned long pos = 0;
        unsigned int new_length = 0;
        ERL_NIF_TERM op,
                     unshift,
                     copy_it,
                     copy_val;

        enif_get_tuple(bucket_env, old, &tb_arity, &old_tpl);
        new_tpl = (ERL_NIF_TERM*)enif_alloc(sizeof(ERL_NIF_TERM) * tb_arity);
        memcpy(new_tpl, old_tpl, sizeof(ERL_NIF_TERM) * tb_arity);

        it = ops;
        ret = enif_make_list(env, 0);

        while (!enif_is_empty_list(env, it)) {
            // Examine the operation.
            enif_get_list_cell(env, it, &op, &it);          // op = hd(it), it = tl(it)
            enif_get_tuple(env, op, &op_arity, &op_tpl);    // op_arity = tuple_size(op), op_tpl = [TplPos1, TplPos2]
            enif_get_ulong(env, op_tpl[0], &pos);           // Tuple position to modify
            unshift = op_tpl[1];                            // Values to unshfit

            // Argument 1 of the operation tuple is position;
            // make sure it's within the bounds of the tuple
            // in the table.
            if (pos <= 0 || pos > tb_arity) {
                ret = enif_make_badarg(env);
                goto bailout;
            }
            
            // Make sure we were passed a list of things to push
            // onto the posth element of the entry
            if (!enif_is_list(env, unshift)) {
                ret = enif_make_badarg(env);
            }

            // Now iterate over unshift, moving its values to
            // the head of new_tpl[pos - 1] one by one
            copy_it = unshift;
            while (!enif_is_empty_list(env, copy_it)) {
                enif_get_list_cell(env, copy_it, &copy_val, &copy_it);
                new_tpl[pos - 1] = enif_make_list_cell(bucket_env, enif_make_copy(bucket_env, copy_val), new_tpl[pos - 1]);
            }
            enif_get_list_length(bucket_env, new_tpl[pos - 1], &new_length);
            ret = enif_make_list_cell(env, enif_make_uint(env, new_length), ret);
        }

        tb->put(entry_key, enif_make_tuple_from_array(bucket_env, new_tpl, tb_arity));

bailout:
        enif_free(new_tpl);
    } else {
        ret = enif_make_badarg(env);
    }
    tb->rwunlock(entry_key);

    return ret;
}

ERL_NIF_TERM NeuralTable::Shift(ErlNifEnv *env, ERL_NIF_TERM table, ERL_NIF_TERM key, ERL_NIF_TERM ops) {
    NeuralTable *tb;
    ERL_NIF_TERM ret, old, it;
    unsigned long int entry_key;
    ErlNifEnv *bucket_env;

    tb = GetTable(env, table);
    if (tb == NULL) {
        return enif_make_badarg(env);
    }

    enif_get_ulong(env, key, &entry_key);

    tb->rwlock(entry_key);
    bucket_env = tb->get_env(entry_key);
    if (tb->find(entry_key, old)) {
        const ERL_NIF_TERM *old_tpl;
        const ERL_NIF_TERM *op_tpl;
        ERL_NIF_TERM *new_tpl;
        int tb_arity = 0,
            op_arity = 0;
        unsigned long pos = 0,
                      count = 0;
        ERL_NIF_TERM op, list, shifted, reclaim;

        enif_get_tuple(bucket_env, old, &tb_arity, &old_tpl);
        new_tpl = (ERL_NIF_TERM*)enif_alloc(tb_arity * sizeof(ERL_NIF_TERM));
        memcpy(new_tpl, old_tpl, sizeof(ERL_NIF_TERM) * tb_arity);

        it = ops;
        ret = enif_make_list(env, 0);
        reclaim = enif_make_list(bucket_env, 0);

        while(!enif_is_empty_list(env, it)) {
            enif_get_list_cell(env, it, &op, &it);
            enif_get_tuple(env, op, &op_arity, &op_tpl);
            enif_get_ulong(env, op_tpl[0], &pos);
            enif_get_ulong(env, op_tpl[1], &count);

            if (pos <= 0 || pos > tb_arity) {
                ret = enif_make_badarg(env);
                goto bailout;
            }

            if (!enif_is_list(env, new_tpl[pos -1])) {
                ret = enif_make_badarg(env);
                goto bailout;
            }

            shifted = enif_make_list(env, 0);
            if (count > 0) {
                ERL_NIF_TERM copy_it = new_tpl[pos - 1],
                             val;
                int i = 0;
                while (i < count && !enif_is_empty_list(bucket_env, copy_it)) {
                    enif_get_list_cell(bucket_env, copy_it, &val, &copy_it);
                    ++i;
                    shifted = enif_make_list_cell(env, enif_make_copy(env, val), shifted);
                    reclaim = enif_make_list_cell(env, val, reclaim);
                }
                new_tpl[pos - 1] = copy_it;
            } else if (count < 0) {
                ERL_NIF_TERM copy_it = new_tpl[pos - 1],
                             val;
                while (!enif_is_empty_list(bucket_env, copy_it)) {
                    enif_get_list_cell(bucket_env, copy_it, &val, &copy_it);
                    shifted = enif_make_list_cell(env, enif_make_copy(env, val), shifted);
                    reclaim = enif_make_list_cell(env, val, reclaim);
                }
                new_tpl[pos - 1] = copy_it;
            }
            ret = enif_make_list_cell(env, shifted, ret);
        }

        tb->put(entry_key, enif_make_tuple_from_array(bucket_env, new_tpl, tb_arity));
        tb->reclaim(entry_key, reclaim);
bailout:
        enif_free(new_tpl);
    } else {
        ret = enif_make_badarg(env);
    }
    tb->rwunlock(entry_key);

    return ret;
}

ERL_NIF_TERM NeuralTable::Swap(ErlNifEnv *env, ERL_NIF_TERM table, ERL_NIF_TERM key, ERL_NIF_TERM ops) {
    NeuralTable *tb;
    ERL_NIF_TERM ret, old, it;
    unsigned long int entry_key;
    ErlNifEnv *bucket_env;

    tb = GetTable(env, table);
    if (tb == NULL) {
        return enif_make_badarg(env);
    }

    enif_get_ulong(env, key, &entry_key);

    tb->rwlock(entry_key);
    bucket_env = tb->get_env(entry_key);
    if (tb->find(entry_key, old)) {
        const ERL_NIF_TERM *old_tpl;
        const ERL_NIF_TERM *op_tpl;
        ERL_NIF_TERM *new_tpl;
        int tb_arity = 0,
            op_arity = 0;
        unsigned long pos = 0;
        ERL_NIF_TERM op, list, shifted, reclaim;

        enif_get_tuple(bucket_env, old, &tb_arity, &old_tpl);
        new_tpl = (ERL_NIF_TERM*)enif_alloc(tb_arity * sizeof(ERL_NIF_TERM));
        memcpy(new_tpl, old_tpl, sizeof(ERL_NIF_TERM) * tb_arity);

        it = ops;
        ret = enif_make_list(env, 0);
        reclaim = enif_make_list(bucket_env, 0);

        while (!enif_is_empty_list(env, it)) {
            enif_get_list_cell(env, it, &op, &it);
            enif_get_tuple(env, op, &op_arity, &op_tpl);
            enif_get_ulong(env, op_tpl[0], &pos);

            if (pos <= 0 || pos > tb_arity) {
                ret = enif_make_badarg(env);
                goto bailout;
            }

            reclaim = enif_make_list_cell(bucket_env, new_tpl[pos - 1], reclaim);
            ret = enif_make_list_cell(env, enif_make_copy(env, new_tpl[pos -1]), ret);
            new_tpl[pos - 1] = enif_make_copy(bucket_env, op_tpl[1]);
        }

        tb->put(entry_key, enif_make_tuple_from_array(bucket_env, new_tpl, tb_arity));
        tb->reclaim(entry_key, reclaim);
bailout:
        enif_free(new_tpl);
    } else {
        ret = enif_make_badarg(env);
    }
    tb->rwunlock(entry_key);

    return ret;
}

ERL_NIF_TERM NeuralTable::Delete(ErlNifEnv *env, ERL_NIF_TERM table, ERL_NIF_TERM key) {
    NeuralTable *tb;
    ERL_NIF_TERM val, ret;
    unsigned long int entry_key;

    tb = GetTable(env, table);
    if (tb == NULL) { return enif_make_badarg(env); }

    enif_get_ulong(env, key, &entry_key);

    tb->rwlock(entry_key);

    if (tb->erase(entry_key, val)) {
        tb->reclaim(entry_key, val);
        ret = enif_make_copy(env, val);
    } else {
        ret = enif_make_atom(env, "undefined");
    }

    tb->rwunlock(entry_key);

    return ret;
}

ERL_NIF_TERM NeuralTable::Empty(ErlNifEnv *env, ERL_NIF_TERM table) {
    NeuralTable *tb;
    int n = 0;

    tb = GetTable(env, table);
    if (tb == NULL) { return enif_make_badarg(env); }

    // First, lock EVERY bucket. We want this to be an isolated operation.
    for (n = 0; n < BUCKET_COUNT; ++n) {
        enif_rwlock_rwlock(tb->locks[n]);
    }

    // Now clear the table
    for (n = 0; n < BUCKET_COUNT; ++n) {
        tb->hash_buckets[n].clear();
        enif_clear_env(tb->env_buckets[n]);
        tb->garbage_cans[n] = 0;
        tb->reclaimable[n] = enif_make_list(tb->env_buckets[n], 0);
    }

    // Now unlock every bucket.
    for (n = 0; n < BUCKET_COUNT; ++n) {
        enif_rwlock_rwunlock(tb->locks[n]);
    }

    return enif_make_atom(env, "ok");
}

ERL_NIF_TERM NeuralTable::Get(ErlNifEnv *env, ERL_NIF_TERM table, ERL_NIF_TERM key) {
    NeuralTable *tb;
    ERL_NIF_TERM ret, val;
    unsigned long int entry_key;

    // Acquire table handle, or quit if the table doesn't exist.
    tb = GetTable(env, table);
    if (tb == NULL) { return enif_make_badarg(env); }

    // Get key value
    enif_get_ulong(env, key, &entry_key);

    // Lock the key
    tb->rlock(entry_key);

    // Read current value
    if (!tb->find(entry_key, val)) {
        ret = enif_make_atom(env, "undefined");
    } else {
        ret = enif_make_copy(env, val);
    }

    tb->runlock(entry_key);

    return ret;
}

ERL_NIF_TERM NeuralTable::Dump(ErlNifEnv *env, ERL_NIF_TERM table) {
    NeuralTable *tb = GetTable(env, table);
    ErlNifPid self;
    ERL_NIF_TERM ret;

    if (tb == NULL) { return enif_make_badarg(env); }

    enif_self(env, &self);

    tb->add_batch_job(self, &NeuralTable::batch_dump);

    return enif_make_atom(env, "$neural_batch_wait");
}

ERL_NIF_TERM NeuralTable::Drain(ErlNifEnv *env, ERL_NIF_TERM table) {
    NeuralTable *tb = GetTable(env, table);
    ErlNifPid self;
    int ret;

    if (tb == NULL) { return enif_make_badarg(env); }

    enif_self(env, &self);

    tb->add_batch_job(self, &NeuralTable::batch_drain);

    return enif_make_atom(env, "$neural_batch_wait");
}

ERL_NIF_TERM NeuralTable::GetKeyPosition(ErlNifEnv *env, ERL_NIF_TERM table) {
    NeuralTable *tb = GetTable(env, table);

    if (tb == NULL) { return enif_make_badarg(env); }
    return enif_make_uint(env, tb->key_pos);
}

ERL_NIF_TERM NeuralTable::GarbageCollect(ErlNifEnv *env, ERL_NIF_TERM table) {
    NeuralTable *tb = GetTable(env, table);
    if (tb == NULL) { return enif_make_badarg(env); }

    enif_cond_signal(tb->gc_cond);

    return enif_make_atom(env, "ok");
}
 
ERL_NIF_TERM NeuralTable::GarbageSize(ErlNifEnv *env, ERL_NIF_TERM table) {
    NeuralTable *tb = GetTable(env, table);
    unsigned long int size = 0;

    if (tb == NULL) { return enif_make_badarg(env); }

    size = tb->garbage_size();

    return enif_make_ulong(env, size);
}

void* NeuralTable::DoGarbageCollection(void *table) {
    NeuralTable *tb = (NeuralTable*)table;

    enif_mutex_lock(tb->gc_mutex);

    while (running.load(memory_order_acquire)) {
        while (running.load(memory_order_acquire) && tb->garbage_size() < RECLAIM_THRESHOLD) {
            enif_cond_wait(tb->gc_cond, tb->gc_mutex);
        }
        tb->gc();
    }

    enif_mutex_unlock(tb->gc_mutex);

    return NULL;
}

void* NeuralTable::DoReclamation(void *table) {
    const int max_eat = 5;
    NeuralTable *tb = (NeuralTable*)table;
    int i = 0, c = 0, t = 0;;
    ERL_NIF_TERM tl, hd;
    ErlNifEnv *env;

    while (running.load(memory_order_acquire)) {
        for (i = 0; i < BUCKET_COUNT; ++i) {
            c = 0;
            t = 0;
            tb->rwlock(i);
            env = tb->get_env(i);
            tl = tb->reclaimable[i];
            while (c++ < max_eat && !enif_is_empty_list(env, tl)) {
                enif_get_list_cell(env, tl, &hd, &tl);
                tb->garbage_cans[i] += estimate_size(env, hd);
                t += tb->garbage_cans[i];
            }
            tb->rwunlock(i);

            if (t >= RECLAIM_THRESHOLD) {
                enif_cond_signal(tb->gc_cond);
            }
        }
        usleep(50000);
    }

    return NULL;
}

void* NeuralTable::DoBatchOperations(void *table) {
    NeuralTable *tb = (NeuralTable*)table;

    enif_mutex_lock(tb->batch_mutex);

    while (running.load(memory_order_acquire)) {
        while (running.load(memory_order_acquire) && tb->batch_jobs.empty()) {
            enif_cond_wait(tb->batch_cond, tb->batch_mutex);
        }
        BatchJob job = tb->batch_jobs.front();
        (tb->*job.fun)(job.pid);
        tb->batch_jobs.pop();
    }

    enif_mutex_unlock(tb->batch_mutex);

    return NULL;
}

void NeuralTable::start_gc() {
    int ret;

    gc_mutex = enif_mutex_create("neural_table_gc");
    gc_cond = enif_cond_create("neural_table_gc");

    ret = enif_thread_create("neural_garbage_collector", &gc_tid, NeuralTable::DoGarbageCollection, (void*)this, NULL);
    if (ret != 0) {
        printf("[neural_gc] Can't create GC thread. Error Code: %d\r\n", ret);
    }

    // Start the reclaimer after the garbage collector.
    ret = enif_thread_create("neural_reclaimer", &rc_tid, NeuralTable::DoReclamation, (void*)this, NULL);
    if (ret != 0) {
        printf("[neural_gc] Can't create reclamation thread. Error Code: %d\r\n", ret);
    }
}

void NeuralTable::stop_gc() {
    enif_cond_signal(gc_cond);
    // Join the reclaimer before the garbage collector.
    enif_thread_join(rc_tid, NULL);
    enif_thread_join(gc_tid, NULL);
}

void NeuralTable::start_batch() {
    int ret; 

    batch_mutex = enif_mutex_create("neural_table_batch");
    batch_cond = enif_cond_create("neural_table_batch");

    ret = enif_thread_create("neural_batcher", &batch_tid, NeuralTable::DoBatchOperations, (void*)this, NULL);
    if (ret != 0) {
        printf("[neural_batch] Can't create batch thread. Error Code: %d\r\n", ret);
    }
}

void NeuralTable::stop_batch() {
    enif_cond_signal(batch_cond);
    enif_thread_join(batch_tid, NULL);
}

void NeuralTable::put(unsigned long int key, ERL_NIF_TERM tuple) {
    ErlNifEnv *env = get_env(key);
    hash_buckets[GET_BUCKET(key)][key] = enif_make_copy(env, tuple);
}

ErlNifEnv* NeuralTable::get_env(unsigned long int key) {
    return env_buckets[GET_BUCKET(key)];
}

bool NeuralTable::find(unsigned long int key, ERL_NIF_TERM &ret) {
    hash_table *bucket = &hash_buckets[GET_BUCKET(key)];
    hash_table::iterator it = bucket->find(key);
    if (bucket->end() == it) {
        return false;
    } else {
        ret = it->second;
        return true;
    }
}

bool NeuralTable::erase(unsigned long int key, ERL_NIF_TERM &val) {
    hash_table *bucket = &hash_buckets[GET_BUCKET(key)];
    hash_table::iterator it = bucket->find(key);
    bool ret = false;
    if (it != bucket->end()) {
        ret = true;
        val = it->second;
        bucket->erase(it);
    }
    return ret;
}

void NeuralTable::add_batch_job(ErlNifPid pid, BatchFunction fun) {
    BatchJob job;
    job.pid = pid;
    job.fun = fun;

    enif_mutex_lock(batch_mutex);
    batch_jobs.push(job);
    enif_mutex_unlock(batch_mutex);

    enif_cond_signal(batch_cond);
}

void NeuralTable::batch_drain(ErlNifPid pid) {
    ErlNifEnv *env = enif_alloc_env();
    ERL_NIF_TERM msg, value;

    value = enif_make_list(env, 0);
    for (int i = 0; i < BUCKET_COUNT; ++i) {
        enif_rwlock_rwlock(locks[i]);

        for (hash_table::iterator it = hash_buckets[i].begin(); it != hash_buckets[i].end(); ++it) {
            value = enif_make_list_cell(env, enif_make_copy(env, it->second), value);
        }
        enif_clear_env(env_buckets[i]);
        hash_buckets[i].clear();
        garbage_cans[i] = 0;
        reclaimable[i] = enif_make_list(env_buckets[i], 0);

        enif_rwlock_rwunlock(locks[i]);
    }

    msg = enif_make_tuple2(env, enif_make_atom(env, "$neural_batch_response"), value);

    enif_send(NULL, &pid, env, msg);

    enif_free_env(env);
}

void NeuralTable::batch_dump(ErlNifPid pid) {
    ErlNifEnv *env = enif_alloc_env();
    ERL_NIF_TERM msg, value;

    value = enif_make_list(env, 0);
    for (int i = 0; i < BUCKET_COUNT; ++i) {
        enif_rwlock_rlock(locks[i]);
        for (hash_table::iterator it = hash_buckets[i].begin(); it != hash_buckets[i].end(); ++it) {
            value = enif_make_list_cell(env, enif_make_copy(env, it->second), value);
        }
        enif_rwlock_runlock(locks[i]);
    }

    msg = enif_make_tuple2(env, enif_make_atom(env, "$neural_batch_response"), value);
    
    enif_send(NULL, &pid, env, msg);

    enif_free_env(env);
}

void NeuralTable::reclaim(unsigned long int key, ERL_NIF_TERM term) {
    int bucket = GET_BUCKET(key);
    ErlNifEnv *env = get_env(key);
    reclaimable[bucket] = enif_make_list_cell(env, term, reclaimable[bucket]);
}

void NeuralTable::gc() {
    ErlNifEnv *fresh    = NULL,
              *old      = NULL;
    hash_table *bucket  = NULL;
    hash_table::iterator it;
    unsigned int gc_curr = 0;

    for (; gc_curr < BUCKET_COUNT; ++gc_curr) {
        bucket = &hash_buckets[gc_curr];
        old = env_buckets[gc_curr];
        fresh = enif_alloc_env();
    
        enif_rwlock_rwlock(locks[gc_curr]);
        for  (it = bucket->begin(); it != bucket->end(); ++it) {
            it->second = enif_make_copy(fresh, it->second);
        }
    
        garbage_cans[gc_curr] = 0;
        env_buckets[gc_curr] = fresh;
        reclaimable[gc_curr] = enif_make_list(fresh, 0);
        enif_free_env(old);
        enif_rwlock_rwunlock(locks[gc_curr]);
    }
}

unsigned long int NeuralTable::garbage_size() {
    unsigned long int size = 0;
    for (int i = 0; i < BUCKET_COUNT; ++i) {
        enif_rwlock_rlock(locks[i]);
        size += garbage_cans[i];
        enif_rwlock_runlock(locks[i]);
    }
    return size;
}
