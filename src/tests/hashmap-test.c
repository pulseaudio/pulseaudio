#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <check.h>
#include <pulsecore/hashmap.h>

struct int_entry {
    int key;
    int value;
};

static unsigned int_trivial_hash_func(const void* pa) {
    int a = *((unsigned*) pa);
    if (a < 0) {
        return -a;
    }
    return a;
}

static int int_compare_func(const void* pa, const void* pb) {
    int a, b;
    a = *((int*) pa);
    b = *((int*) pb);
    if (a < b) {
        return -1;
    }
    if (a == b) {
        return 0;
    }
    return 1;
}

/* single_key_test exercises basic hashmap functionality on a single key. */
START_TEST(single_key_test)
    {
        pa_hashmap* map;
        struct int_entry entry;
        int lookup_key;
        int put_ret;
        void* get_ret;
        unsigned size_ret;

        entry.key = 0;
        entry.value = 0;

        lookup_key = 0;

        map = pa_hashmap_new(int_trivial_hash_func, int_compare_func);

        if ((put_ret = pa_hashmap_put(map, &entry.key, &entry)) != 0) {
            ck_abort_msg("Hashmap rejected k=0/v=0; got %d, want %d", put_ret, 0);
        }

        if ((size_ret = pa_hashmap_size(map)) != 1) {
            ck_abort_msg("Hashmap reported wrong size; got %u, want 1", size_ret);
        }

        if ((get_ret = pa_hashmap_get(map, &lookup_key)) != &entry) {
            ck_abort_msg("Got wrong value from hashmap for k=0; got %p, want %p", get_ret, &entry);
        }

        if ((put_ret = pa_hashmap_put(map, &entry.key, &entry)) == 0) {
            ck_abort_msg("Hashmap allowed duplicate key for k=0; got %d, want non-zero", put_ret);
        }

        if ((size_ret = pa_hashmap_size(map)) != 1) {
            ck_abort_msg("Hashmap reported wrong size; got %u, want 1", size_ret);
        }

        if ((get_ret = pa_hashmap_remove(map, &lookup_key)) != &entry) {
            ck_abort_msg("Hashmap returned wrong value during free; got %p, want %p", get_ret, &entry);
        }

        if ((size_ret = pa_hashmap_size(map)) != 0) {
            ck_abort_msg("Hashmap reported wrong size; got %u, want 1", size_ret);
        }

        pa_hashmap_free(map);
    }
END_TEST

/* remove_all_test checks that pa_hashmap_remove_all really removes all entries
 * from the map.*/
START_TEST(remove_all_test)
    {
        pa_hashmap* map;
        struct int_entry entries[1000];
        unsigned size;

        for (int i = 0; i < 1000; i++) {
            entries[i].key = i;
            entries[i].value = i;
        }

        map = pa_hashmap_new(int_trivial_hash_func, int_compare_func);

        for (int i = 0; i < 1000; i++) {
            pa_hashmap_put(map, &entries[i].key, &entries[i]);
        }

        if ((size = pa_hashmap_size(map)) != 1000) {
            ck_abort_msg("Hashmap has wrong size; got %u, want 1000", size);
        }

        pa_hashmap_remove_all(map);

        if ((size = pa_hashmap_size(map)) != 0) {
            ck_abort_msg("Hashmap has wrong size; got %u, want 0", size);
        }

        pa_hashmap_free(map);
    }
END_TEST

/* fill_all_buckets hits the hashmap with enough keys to exercise the bucket
 * linked list for every bucket. */
START_TEST(fill_all_buckets)
    {
        pa_hashmap* map;
        struct int_entry entries[1000];
        int lookup_keys[1000]; /* Don't share addresses with insertion keys */

        map = pa_hashmap_new(int_trivial_hash_func, int_compare_func);

        for (int i = 0; i < 1000; i++) {
            entries[i].key = i;
            lookup_keys[i] = i;
            entries[i].value = i;
        }

        for (int i = 0; i < 1000; i++) {
            int put_ret;
            unsigned size_ret;

            if ((put_ret = pa_hashmap_put(map, &entries[i].key, &entries[i])) != 0) {
                ck_abort_msg("Unexpected failure putting k=%d v=%d into the map", entries[i].key, entries[i].value);
            }

            if ((size_ret = pa_hashmap_size(map)) != i + 1) {
                ck_abort_msg("Hashmap reported wrong size; got %u, want %d", size_ret, i);
            }
        }

        for (int i = 0; i < 1000; i++) {
            unsigned size_ret;
            int* k;
            struct int_entry* v;

            k = lookup_keys + i;

            v = (struct int_entry*) pa_hashmap_remove(map, k);
            if (v == NULL) {
                ck_abort_msg("Hashmap returned NULL for k=%d; wanted nonnull", *k);
            }
            if ((*v).value != i) {
                ck_abort_msg("Hashmap returned wrong value for k=%d; got %d, want %d", *k, (*v).value, i);
            }

            if ((size_ret = pa_hashmap_size(map)) != 1000 - i - 1) {
                ck_abort_msg("Hashmap reported wrong size; got %u, want %d", size_ret, 1000 - i);
            }
        }

        pa_hashmap_free(map);
    }
END_TEST

/* iterate_test exercises the iteration list maintained by the hashtable. */
START_TEST(iterate_test)
    {
        pa_hashmap* map;
        struct int_entry entries[1000];
        void* state;
        struct int_entry* v;
        int expected;

        for (int i = 0; i < 1000; i++) {
            entries[i].key = i;
            entries[i].value = i;
        }

        map = pa_hashmap_new(int_trivial_hash_func, int_compare_func);

        for (int i = 0; i < 1000; i++) {
            if (pa_hashmap_put(map, &(entries[i].key), &(entries[i])) != 0) {
                ck_abort_msg("Unexpected failure putting k=%d v=%d into the map", entries[i].key, entries[i].value);
            }
        }

        expected = 0;
        PA_HASHMAP_FOREACH (v, map, state) {
            if ((*v).value != expected) {
                ck_abort_msg("Got bad order iterating over hashmap: got %d, want %d", v->value, expected);
            }
            expected++;
        }

        expected = 999;
        PA_HASHMAP_FOREACH_BACKWARDS (v, map, state) {
            if ((*v).value != expected) {
                ck_abort_msg("Got bad order iterating over hashmap: got %d, want %d", v->value, expected);
            }
            expected--;
        }

        /* Now empty out the hashmap.  The iteration list should be empty. */
        for(int i = 0; i < 1000; i++) {
          pa_hashmap_remove(map, &(entries[i].key));
        }

        PA_HASHMAP_FOREACH(v, map, state) {
          ck_abort_msg("Iteration over empty map returned entries");
        }

        /* Now add one element back. The iteration list should only contain this
         * one element, even though the entry nodes are reused. */
        if(pa_hashmap_put(map, &(entries[0].key), &(entries[0])) != 0) {
          ck_abort_msg("Unexpected failure putting k=%d v=%d into the map", entries[0].key, entries[0].value);
        }

        expected = 0;
        PA_HASHMAP_FOREACH(v, map, state) {
           if ((*v).value != expected) {
                ck_abort_msg("Got bad order iterating over hashmap: got %d, want %d", v->value, expected);
            }
           expected++;
        }
        if (expected != 1) {
          ck_abort_msg("Got too many elements while iterating: got %d, want 1", expected);
        }

        pa_hashmap_free(map);
    }
END_TEST

int main(int argc, char** argv) {
    int failed = 0;
    Suite* s;
    TCase* tc;
    SRunner* sr;

    s = suite_create("HashMap");
    tc = tcase_create("hashmap");
    tcase_add_test(tc, single_key_test);
    tcase_add_test(tc, remove_all_test);
    tcase_add_test(tc, fill_all_buckets);
    tcase_add_test(tc, iterate_test);
    suite_add_tcase(s, tc);

    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    if (failed > 0) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
