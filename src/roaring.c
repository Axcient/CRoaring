#include <assert.h>
#include <roaring/array_util.h>
#include <roaring/roaring.h>
#include <roaring/roaring_array.h>
#include <roaring/bitset_util.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

extern inline bool roaring_bitmap_contains(const roaring_bitmap_t *r,
                                           uint32_t val);
extern inline bool roaring_bitmap_get_copy_on_write(const roaring_bitmap_t* r);
extern inline void roaring_bitmap_set_copy_on_write(roaring_bitmap_t* r, bool cow);

static inline bool is_cow(const roaring_bitmap_t *r) {
    return r->high_low_container.flags & ROARING_FLAG_COW;
}
static inline bool is_frozen(const roaring_bitmap_t *r) {
    return r->high_low_container.flags & ROARING_FLAG_FROZEN;
}

// this is like roaring_bitmap_add, but it populates pointer arguments in such a
// way
// that we can recover the container touched, which, in turn can be used to
// accelerate some functions (when you repeatedly need to add to the same
// container)
static inline void *containerptr_roaring_bitmap_add(roaring_bitmap_t *r,
                                                    uint32_t val,
                                                    uint8_t *typecode,
                                                    int *index) {
    uint16_t hb = val >> 16;
    const int i = ra_get_index(&r->high_low_container, hb);
    if (i >= 0) {
        ra_unshare_container_at_index(&r->high_low_container, i);
        void *container =
            ra_get_container_at_index(&r->high_low_container, i, typecode);
        uint8_t newtypecode = *typecode;
        void *container2 =
            container_add(container, val & 0xFFFF, *typecode, &newtypecode);
        *index = i;
        if (container2 != container) {
            container_free(container, *typecode);
            ra_set_container_at_index(&r->high_low_container, i, container2,
                                      newtypecode);
            *typecode = newtypecode;
            return container2;
        } else {
            return container;
        }
    } else {
        array_container_t *newac = array_container_create();
        void *container = container_add(newac, val & 0xFFFF,
                                        ARRAY_CONTAINER_TYPE_CODE, typecode);
        // we could just assume that it stays an array container
        ra_insert_new_key_value_at(&r->high_low_container, -i - 1, hb,
                                   container, *typecode);
        *index = -i - 1;
        return container;
    }
}

roaring_bitmap_t *roaring_bitmap_create() {
    roaring_bitmap_t *ans =
        (roaring_bitmap_t *)malloc(sizeof(roaring_bitmap_t));
    if (!ans) {
        return NULL;
    }
    ra_init(&ans->high_low_container);
    return ans;
}

roaring_bitmap_t *roaring_bitmap_create_with_capacity(uint32_t cap) {
    roaring_bitmap_t *ans =
        (roaring_bitmap_t *)malloc(sizeof(roaring_bitmap_t));
    if (!ans) {
        return NULL;
    }
    bool is_ok = ra_init_with_capacity(&ans->high_low_container, cap);
    if (!is_ok) {
        free(ans);
        return NULL;
    }
    return ans;
}

void roaring_bitmap_add_many(roaring_bitmap_t *r, size_t n_args,
                             const uint32_t *vals) {
    void *container = NULL;  // hold value of last container touched
    uint8_t typecode = 0;    // typecode of last container touched
    uint32_t prev = 0;       // previous valued inserted
    size_t i = 0;            // index of value
    int containerindex = 0;
    if (n_args == 0) return;
    uint32_t val;
    memcpy(&val, vals + i, sizeof(val));
    container =
        containerptr_roaring_bitmap_add(r, val, &typecode, &containerindex);
    prev = val;
    i++;
    for (; i < n_args; i++) {
        memcpy(&val, vals + i, sizeof(val));
        if (((prev ^ val) >> 16) ==
            0) {  // no need to seek the container, it is at hand
            // because we already have the container at hand, we can do the
            // insertion
            // automatically, bypassing the roaring_bitmap_add call
            uint8_t newtypecode = typecode;
            void *container2 =
                container_add(container, val & 0xFFFF, typecode, &newtypecode);
            if (container2 != container) {  // rare instance when we need to
                                            // change the container type
                container_free(container, typecode);
                ra_set_container_at_index(&r->high_low_container,
                                          containerindex, container2,
                                          newtypecode);
                typecode = newtypecode;
                container = container2;
            }
        } else {
            container = containerptr_roaring_bitmap_add(r, val, &typecode,
                                                        &containerindex);
        }
        prev = val;
    }
}

roaring_bitmap_t *roaring_bitmap_of_ptr(size_t n_args, const uint32_t *vals) {
    roaring_bitmap_t *answer = roaring_bitmap_create();
    roaring_bitmap_add_many(answer, n_args, vals);
    return answer;
}

roaring_bitmap_t *roaring_bitmap_of(size_t n_args, ...) {
    // todo: could be greatly optimized but we do not expect this call to ever
    // include long lists
    roaring_bitmap_t *answer = roaring_bitmap_create();
    va_list ap;
    va_start(ap, n_args);
    for (size_t i = 1; i <= n_args; i++) {
        uint32_t val = va_arg(ap, uint32_t);
        roaring_bitmap_add(answer, val);
    }
    va_end(ap);
    return answer;
}

static inline uint32_t minimum_uint32(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

static inline uint64_t minimum_uint64(uint64_t a, uint64_t b) {
    return (a < b) ? a : b;
}

roaring_bitmap_t *roaring_bitmap_from_range(uint64_t min, uint64_t max,
                                            uint32_t step) {
    if(max >= UINT64_C(0x100000000)) {
        max = UINT64_C(0x100000000);
    }
    if (step == 0) return NULL;
    if (max <= min) return NULL;
    roaring_bitmap_t *answer = roaring_bitmap_create();
    if (step >= (1 << 16)) {
        for (uint32_t value = (uint32_t)min; value < max; value += step) {
            roaring_bitmap_add(answer, value);
        }
        return answer;
    }
    uint64_t min_tmp = min;
    do {
        uint32_t key = (uint32_t)min_tmp >> 16;
        uint32_t container_min = min_tmp & 0xFFFF;
        uint32_t container_max = (uint32_t)minimum_uint64(max - (key << 16), 1 << 16);
        uint8_t type;
        void *container = container_from_range(&type, container_min,
                                               container_max, (uint16_t)step);
        ra_append(&answer->high_low_container, key, container, type);
        uint32_t gap = container_max - container_min + step - 1;
        min_tmp += gap - (gap % step);
    } while (min_tmp < max);
    // cardinality of bitmap will be ((uint64_t) max - min + step - 1 ) / step
    return answer;
}

void roaring_bitmap_add_range_closed(roaring_bitmap_t *ra, uint32_t min, uint32_t max) {
    if (min > max) {
        return;
    }

    uint32_t min_key = min >> 16;
    uint32_t max_key = max >> 16;

    int32_t num_required_containers = max_key - min_key + 1;
    int32_t suffix_length = count_greater(ra->high_low_container.keys,
                                          ra->high_low_container.size,
                                          max_key);
    int32_t prefix_length = count_less(ra->high_low_container.keys,
                                       ra->high_low_container.size - suffix_length,
                                       min_key);
    int32_t common_length = ra->high_low_container.size - prefix_length - suffix_length;

    if (num_required_containers > common_length) {
        ra_shift_tail(&ra->high_low_container, suffix_length,
                      num_required_containers - common_length);
    }

    int32_t src = prefix_length + common_length - 1;
    int32_t dst = ra->high_low_container.size - suffix_length - 1;
    for (uint32_t key = max_key; key != min_key-1; key--) { // beware of min_key==0
        uint32_t container_min = (min_key == key) ? (min & 0xffff) : 0;
        uint32_t container_max = (max_key == key) ? (max & 0xffff) : 0xffff;
        void* new_container;
        uint8_t new_type;

        if (src >= 0 && ra->high_low_container.keys[src] == key) {
            ra_unshare_container_at_index(&ra->high_low_container, src);
            new_container = container_add_range(ra->high_low_container.containers[src],
                                                ra->high_low_container.typecodes[src],
                                                container_min, container_max, &new_type);
            if (new_container != ra->high_low_container.containers[src]) {
                container_free(ra->high_low_container.containers[src],
                               ra->high_low_container.typecodes[src]);
            }
            src--;
        } else {
            new_container = container_from_range(&new_type, container_min,
                                                 container_max+1, 1);
        }
        ra_replace_key_and_container_at_index(&ra->high_low_container, dst,
                                              key, new_container, new_type);
        dst--;
    }
}

void roaring_bitmap_remove_range_closed(roaring_bitmap_t *ra, uint32_t min, uint32_t max) {
    if (min > max) {
        return;
    }

    uint32_t min_key = min >> 16;
    uint32_t max_key = max >> 16;

    int32_t src = count_less(ra->high_low_container.keys, ra->high_low_container.size, min_key);
    int32_t dst = src;
    while (src < ra->high_low_container.size && ra->high_low_container.keys[src] <= max_key) {
        uint32_t container_min = (min_key == ra->high_low_container.keys[src]) ? (min & 0xffff) : 0;
        uint32_t container_max = (max_key == ra->high_low_container.keys[src]) ? (max & 0xffff) : 0xffff;
        ra_unshare_container_at_index(&ra->high_low_container, src);
        void *new_container;
        uint8_t new_type;
        new_container = container_remove_range(ra->high_low_container.containers[src],
                                               ra->high_low_container.typecodes[src],
                                               container_min, container_max,
                                               &new_type);
        if (new_container != ra->high_low_container.containers[src]) {
            container_free(ra->high_low_container.containers[src],
                           ra->high_low_container.typecodes[src]);
        }
        if (new_container) {
            ra_replace_key_and_container_at_index(&ra->high_low_container, dst,
                                                  ra->high_low_container.keys[src],
                                                  new_container, new_type);
            dst++;
        }
        src++;
    }
    if (src > dst) {
        ra_shift_tail(&ra->high_low_container, ra->high_low_container.size - src, dst - src);
    }
}

extern inline void roaring_bitmap_add_range(roaring_bitmap_t *ra, uint64_t min, uint64_t max);
extern inline void roaring_bitmap_remove_range(roaring_bitmap_t *ra, uint64_t min, uint64_t max);

void roaring_bitmap_printf(const roaring_bitmap_t *ra) {
    printf("{");
    for (int i = 0; i < ra->high_low_container.size; ++i) {
        container_printf_as_uint32_array(
            ra->high_low_container.containers[i],
            ra->high_low_container.typecodes[i],
            ((uint32_t)ra->high_low_container.keys[i]) << 16);
        if (i + 1 < ra->high_low_container.size) printf(",");
    }
    printf("}");
}

void roaring_bitmap_printf_describe(const roaring_bitmap_t *ra) {
    printf("{");
    for (int i = 0; i < ra->high_low_container.size; ++i) {
        printf("%d: %s (%d)", ra->high_low_container.keys[i],
               get_full_container_name(ra->high_low_container.containers[i],
                                       ra->high_low_container.typecodes[i]),
               container_get_cardinality(ra->high_low_container.containers[i],
                                         ra->high_low_container.typecodes[i]));
        if (ra->high_low_container.typecodes[i] == SHARED_CONTAINER_TYPE_CODE) {
            printf(
                "(shared count = %" PRIu32 " )",
                ((shared_container_t *)(ra->high_low_container.containers[i]))
                    ->counter);
        }

        if (i + 1 < ra->high_low_container.size) printf(", ");
    }
    printf("}");
}

typedef struct min_max_sum_s {
    uint32_t min;
    uint32_t max;
    uint64_t sum;
} min_max_sum_t;

static bool min_max_sum_fnc(uint32_t value, void *param) {
    min_max_sum_t *mms = (min_max_sum_t *)param;
    if (value > mms->max) mms->max = value;
    if (value < mms->min) mms->min = value;
    mms->sum += value;
    return true;  // we always process all data points
}

/**
*  (For advanced users.)
* Collect statistics about the bitmap
*/
void roaring_bitmap_statistics(const roaring_bitmap_t *ra,
                               roaring_statistics_t *stat) {
    memset(stat, 0, sizeof(*stat));
    stat->n_containers = ra->high_low_container.size;
    stat->cardinality = roaring_bitmap_get_cardinality(ra);
    min_max_sum_t mms;
    mms.min = UINT32_C(0xFFFFFFFF);
    mms.max = UINT32_C(0);
    mms.sum = 0;
    roaring_iterate(ra, &min_max_sum_fnc, &mms);
    stat->min_value = mms.min;
    stat->max_value = mms.max;
    stat->sum_value = mms.sum;

    for (int i = 0; i < ra->high_low_container.size; ++i) {
        uint8_t truetype =
            get_container_type(ra->high_low_container.containers[i],
                               ra->high_low_container.typecodes[i]);
        uint32_t card =
            container_get_cardinality(ra->high_low_container.containers[i],
                                      ra->high_low_container.typecodes[i]);
        uint32_t sbytes =
            container_size_in_bytes(ra->high_low_container.containers[i],
                                    ra->high_low_container.typecodes[i]);
        switch (truetype) {
            case BITSET_CONTAINER_TYPE_CODE:
                stat->n_bitset_containers++;
                stat->n_values_bitset_containers += card;
                stat->n_bytes_bitset_containers += sbytes;
                break;
            case ARRAY_CONTAINER_TYPE_CODE:
                stat->n_array_containers++;
                stat->n_values_array_containers += card;
                stat->n_bytes_array_containers += sbytes;
                break;
            case RUN_CONTAINER_TYPE_CODE:
                stat->n_run_containers++;
                stat->n_values_run_containers += card;
                stat->n_bytes_run_containers += sbytes;
                break;
            default:
                assert(false);
                __builtin_unreachable();
        }
    }
}

roaring_bitmap_t *roaring_bitmap_copy(const roaring_bitmap_t *r) {
    roaring_bitmap_t *ans =
        (roaring_bitmap_t *)malloc(sizeof(roaring_bitmap_t));
    if (!ans) {
        return NULL;
    }
    bool is_ok = ra_copy(&r->high_low_container, &ans->high_low_container,
                         is_cow(r));
    if (!is_ok) {
        free(ans);
        return NULL;
    }
    roaring_bitmap_set_copy_on_write(ans, is_cow(r));
    return ans;
}

bool roaring_bitmap_overwrite(roaring_bitmap_t *dest,
                                     const roaring_bitmap_t *src) {
    return ra_overwrite(&src->high_low_container, &dest->high_low_container,
                        is_cow(src));
}

void roaring_bitmap_free(const roaring_bitmap_t *r) {
    if (!is_frozen(r)) {
      ra_clear((roaring_array_t*)&r->high_low_container);
    }
    free((roaring_bitmap_t*)r);
}

void roaring_bitmap_clear(roaring_bitmap_t *r) {
  ra_reset(&r->high_low_container);
}

void roaring_bitmap_add(roaring_bitmap_t *r, uint32_t val) {
    const uint16_t hb = val >> 16;
    const int i = ra_get_index(&r->high_low_container, hb);
    uint8_t typecode;
    if (i >= 0) {
        ra_unshare_container_at_index(&r->high_low_container, i);
        void *container =
            ra_get_container_at_index(&r->high_low_container, i, &typecode);
        uint8_t newtypecode = typecode;
        void *container2 =
            container_add(container, val & 0xFFFF, typecode, &newtypecode);
        if (container2 != container) {
            container_free(container, typecode);
            ra_set_container_at_index(&r->high_low_container, i, container2,
                                      newtypecode);
        }
    } else {
        array_container_t *newac = array_container_create();
        void *container = container_add(newac, val & 0xFFFF,
                                        ARRAY_CONTAINER_TYPE_CODE, &typecode);
        // we could just assume that it stays an array container
        ra_insert_new_key_value_at(&r->high_low_container, -i - 1, hb,
                                   container, typecode);
    }
}

bool roaring_bitmap_add_checked(roaring_bitmap_t *r, uint32_t val) {
    const uint16_t hb = val >> 16;
    const int i = ra_get_index(&r->high_low_container, hb);
    uint8_t typecode;
    bool result = false;
    if (i >= 0) {
        ra_unshare_container_at_index(&r->high_low_container, i);
        void *container =
            ra_get_container_at_index(&r->high_low_container, i, &typecode);

        const int oldCardinality =
            container_get_cardinality(container, typecode);

        uint8_t newtypecode = typecode;
        void *container2 =
            container_add(container, val & 0xFFFF, typecode, &newtypecode);
        if (container2 != container) {
            container_free(container, typecode);
            ra_set_container_at_index(&r->high_low_container, i, container2,
                                      newtypecode);
            result = true;
        } else {
            const int newCardinality =
                container_get_cardinality(container, newtypecode);

            result = oldCardinality != newCardinality;
        }
    } else {
        array_container_t *newac = array_container_create();
        void *container = container_add(newac, val & 0xFFFF,
                                        ARRAY_CONTAINER_TYPE_CODE, &typecode);
        // we could just assume that it stays an array container
        ra_insert_new_key_value_at(&r->high_low_container, -i - 1, hb,
                                   container, typecode);
        result = true;
    }

    return result;
}

void roaring_bitmap_remove(roaring_bitmap_t *r, uint32_t val) {
    const uint16_t hb = val >> 16;
    const int i = ra_get_index(&r->high_low_container, hb);
    uint8_t typecode;
    if (i >= 0) {
        ra_unshare_container_at_index(&r->high_low_container, i);
        void *container =
            ra_get_container_at_index(&r->high_low_container, i, &typecode);
        uint8_t newtypecode = typecode;
        void *container2 =
            container_remove(container, val & 0xFFFF, typecode, &newtypecode);
        if (container2 != container) {
            container_free(container, typecode);
            ra_set_container_at_index(&r->high_low_container, i, container2,
                                      newtypecode);
        }
        if (container_get_cardinality(container2, newtypecode) != 0) {
            ra_set_container_at_index(&r->high_low_container, i, container2,
                                      newtypecode);
        } else {
            ra_remove_at_index_and_free(&r->high_low_container, i);
        }
    }
}

bool roaring_bitmap_remove_checked(roaring_bitmap_t *r, uint32_t val) {
    const uint16_t hb = val >> 16;
    const int i = ra_get_index(&r->high_low_container, hb);
    uint8_t typecode;
    bool result = false;
    if (i >= 0) {
        ra_unshare_container_at_index(&r->high_low_container, i);
        void *container =
            ra_get_container_at_index(&r->high_low_container, i, &typecode);

        const int oldCardinality =
            container_get_cardinality(container, typecode);

        uint8_t newtypecode = typecode;
        void *container2 =
            container_remove(container, val & 0xFFFF, typecode, &newtypecode);
        if (container2 != container) {
            container_free(container, typecode);
            ra_set_container_at_index(&r->high_low_container, i, container2,
                                      newtypecode);
        }

        const int newCardinality =
            container_get_cardinality(container2, newtypecode);

        if (newCardinality != 0) {
            ra_set_container_at_index(&r->high_low_container, i, container2,
                                      newtypecode);
        } else {
            ra_remove_at_index_and_free(&r->high_low_container, i);
        }

        result = oldCardinality != newCardinality;
    }
    return result;
}

void roaring_bitmap_remove_many(roaring_bitmap_t *r, size_t n_args,
                                const uint32_t *vals) {
    if (n_args == 0 || r->high_low_container.size == 0) {
        return;
    }
    int32_t pos = -1; // position of the container used in the previous iteration
    for (size_t i = 0; i < n_args; i++) {
        uint16_t key = (uint16_t)(vals[i] >> 16);
        if (pos < 0 || key != r->high_low_container.keys[pos]) {
            pos = ra_get_index(&r->high_low_container, key);
        }
        if (pos >= 0) {
            uint8_t new_typecode;
            void *new_container;
            new_container = container_remove(r->high_low_container.containers[pos],
                                             vals[i] & 0xffff,
                                             r->high_low_container.typecodes[pos],
                                             &new_typecode);
            if (new_container != r->high_low_container.containers[pos]) {
                container_free(r->high_low_container.containers[pos],
                               r->high_low_container.typecodes[pos]);
                ra_replace_key_and_container_at_index(&r->high_low_container,
                                                      pos, key, new_container,
                                                      new_typecode);
            }
            if (!container_nonzero_cardinality(new_container, new_typecode)) {
                container_free(new_container, new_typecode);
                ra_remove_at_index(&r->high_low_container, pos);
                pos = -1;
            }
        }
    }
}

// there should be some SIMD optimizations possible here
roaring_bitmap_t *roaring_bitmap_and(const roaring_bitmap_t *x1,
                                     const roaring_bitmap_t *x2) {
    uint8_t container_result_type = 0;
    const int length1 = x1->high_low_container.size,
              length2 = x2->high_low_container.size;
    uint32_t neededcap = length1 > length2 ? length2 : length1;
    roaring_bitmap_t *answer = roaring_bitmap_create_with_capacity(neededcap);
    roaring_bitmap_set_copy_on_write(answer, is_cow(x1) && is_cow(x2));

    int pos1 = 0, pos2 = 0;

    while (pos1 < length1 && pos2 < length2) {
        const uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
        const uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, pos2);

        if (s1 == s2) {
            uint8_t container_type_1, container_type_2;
            void *c1 = ra_get_container_at_index(&x1->high_low_container, pos1,
                                                 &container_type_1);
            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            void *c = container_and(c1, container_type_1, c2, container_type_2,
                                    &container_result_type);
            if (container_nonzero_cardinality(c, container_result_type)) {
                ra_append(&answer->high_low_container, s1, c,
                          container_result_type);
            } else {
                container_free(
                    c, container_result_type);  // otherwise:memory leak!
            }
            ++pos1;
            ++pos2;
        } else if (s1 < s2) {  // s1 < s2
            pos1 = ra_advance_until(&x1->high_low_container, s2, pos1);
        } else {  // s1 > s2
            pos2 = ra_advance_until(&x2->high_low_container, s1, pos2);
        }
    }
    return answer;
}

/**
 * Compute the union of 'number' bitmaps.
 */
roaring_bitmap_t *roaring_bitmap_or_many(size_t number,
                                         const roaring_bitmap_t **x) {
    if (number == 0) {
        return roaring_bitmap_create();
    }
    if (number == 1) {
        return roaring_bitmap_copy(x[0]);
    }
    roaring_bitmap_t *answer =
        roaring_bitmap_lazy_or(x[0], x[1], LAZY_OR_BITSET_CONVERSION);
    for (size_t i = 2; i < number; i++) {
        roaring_bitmap_lazy_or_inplace(answer, x[i], LAZY_OR_BITSET_CONVERSION);
    }
    roaring_bitmap_repair_after_lazy(answer);
    return answer;
}

/**
 * Compute the xor of 'number' bitmaps.
 */
roaring_bitmap_t *roaring_bitmap_xor_many(size_t number,
                                          const roaring_bitmap_t **x) {
    if (number == 0) {
        return roaring_bitmap_create();
    }
    if (number == 1) {
        return roaring_bitmap_copy(x[0]);
    }
    roaring_bitmap_t *answer = roaring_bitmap_lazy_xor(x[0], x[1]);
    for (size_t i = 2; i < number; i++) {
        roaring_bitmap_lazy_xor_inplace(answer, x[i]);
    }
    roaring_bitmap_repair_after_lazy(answer);
    return answer;
}

// inplace and (modifies its first argument).
void roaring_bitmap_and_inplace(roaring_bitmap_t *x1,
                                const roaring_bitmap_t *x2) {
    if (x1 == x2) return;
    int pos1 = 0, pos2 = 0, intersection_size = 0;
    const int length1 = ra_get_size(&x1->high_low_container);
    const int length2 = ra_get_size(&x2->high_low_container);

    // any skipped-over or newly emptied containers in x1
    // have to be freed.
    while (pos1 < length1 && pos2 < length2) {
        const uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
        const uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, pos2);

        if (s1 == s2) {
            uint8_t typecode1, typecode2, typecode_result;
            void *c1 = ra_get_container_at_index(&x1->high_low_container, pos1,
                                                 &typecode1);
            c1 = get_writable_copy_if_shared(c1, &typecode1);
            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &typecode2);
            void *c =
                container_iand(c1, typecode1, c2, typecode2, &typecode_result);
            if (c != c1) {  // in this instance a new container was created, and
                            // we need to free the old one
                container_free(c1, typecode1);
            }
            if (container_nonzero_cardinality(c, typecode_result)) {
                ra_replace_key_and_container_at_index(&x1->high_low_container,
                                                      intersection_size, s1, c,
                                                      typecode_result);
                intersection_size++;
            } else {
                container_free(c, typecode_result);
            }
            ++pos1;
            ++pos2;
        } else if (s1 < s2) {
            pos1 = ra_advance_until_freeing(&x1->high_low_container, s2, pos1);
        } else {  // s1 > s2
            pos2 = ra_advance_until(&x2->high_low_container, s1, pos2);
        }
    }

    // if we ended early because x2 ran out, then all remaining in x1 should be
    // freed
    while (pos1 < length1) {
        container_free(x1->high_low_container.containers[pos1],
                       x1->high_low_container.typecodes[pos1]);
        ++pos1;
    }

    // all containers after this have either been copied or freed
    ra_downsize(&x1->high_low_container, intersection_size);
}

roaring_bitmap_t *roaring_bitmap_or(const roaring_bitmap_t *x1,
                                    const roaring_bitmap_t *x2) {
    uint8_t container_result_type = 0;
    const int length1 = x1->high_low_container.size,
              length2 = x2->high_low_container.size;
    if (0 == length1) {
        return roaring_bitmap_copy(x2);
    }
    if (0 == length2) {
        return roaring_bitmap_copy(x1);
    }
    roaring_bitmap_t *answer =
        roaring_bitmap_create_with_capacity(length1 + length2);
    roaring_bitmap_set_copy_on_write(answer, is_cow(x1) && is_cow(x2));
    int pos1 = 0, pos2 = 0;
    uint8_t container_type_1, container_type_2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
    while (true) {
        if (s1 == s2) {
            void *c1 = ra_get_container_at_index(&x1->high_low_container, pos1,
                                                 &container_type_1);
            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            void *c = container_or(c1, container_type_1, c2, container_type_2,
                                   &container_result_type);
            // since we assume that the initial containers are non-empty, the
            // result here
            // can only be non-empty
            ra_append(&answer->high_low_container, s1, c,
                      container_result_type);
            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);

        } else if (s1 < s2) {  // s1 < s2
            void *c1 = ra_get_container_at_index(&x1->high_low_container, pos1,
                                                 &container_type_1);
            // c1 = container_clone(c1, container_type_1);
            c1 =
                get_copy_of_container(c1, &container_type_1, is_cow(x1));
            if (is_cow(x1)) {
                ra_set_container_at_index(&x1->high_low_container, pos1, c1,
                                          container_type_1);
            }
            ra_append(&answer->high_low_container, s1, c1, container_type_1);
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);

        } else {  // s1 > s2
            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            // c2 = container_clone(c2, container_type_2);
            c2 =
                get_copy_of_container(c2, &container_type_2, is_cow(x2));
            if (is_cow(x2)) {
                ra_set_container_at_index(&x2->high_low_container, pos2, c2,
                                          container_type_2);
            }
            ra_append(&answer->high_low_container, s2, c2, container_type_2);
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(&answer->high_low_container,
                             &x2->high_low_container, pos2, length2,
                             is_cow(x2));
    } else if (pos2 == length2) {
        ra_append_copy_range(&answer->high_low_container,
                             &x1->high_low_container, pos1, length1,
                             is_cow(x1));
    }
    return answer;
}

// inplace or (modifies its first argument).
void roaring_bitmap_or_inplace(roaring_bitmap_t *x1,
                               const roaring_bitmap_t *x2) {
    uint8_t container_result_type = 0;
    int length1 = x1->high_low_container.size;
    const int length2 = x2->high_low_container.size;

    if (0 == length2) return;

    if (0 == length1) {
        roaring_bitmap_overwrite(x1, x2);
        return;
    }
    int pos1 = 0, pos2 = 0;
    uint8_t container_type_1, container_type_2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
    while (true) {
        if (s1 == s2) {
            void *c1 = ra_get_container_at_index(&x1->high_low_container, pos1,
                                                 &container_type_1);
            if (!container_is_full(c1, container_type_1)) {
                c1 = get_writable_copy_if_shared(c1, &container_type_1);

                void *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                     pos2, &container_type_2);
                void *c =
                    container_ior(c1, container_type_1, c2, container_type_2,
                                  &container_result_type);
                if (c !=
                    c1) {  // in this instance a new container was created, and
                           // we need to free the old one
                    container_free(c1, container_type_1);
                }

                ra_set_container_at_index(&x1->high_low_container, pos1, c,
                                          container_result_type);
            }
            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);

        } else if (s1 < s2) {  // s1 < s2
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);

        } else {  // s1 > s2
            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            c2 =
                get_copy_of_container(c2, &container_type_2, is_cow(x2));
            if (is_cow(x2)) {
                ra_set_container_at_index(&x2->high_low_container, pos2, c2,
                                          container_type_2);
            }

            // void *c2_clone = container_clone(c2, container_type_2);
            ra_insert_new_key_value_at(&x1->high_low_container, pos1, s2, c2,
                                       container_type_2);
            pos1++;
            length1++;
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(&x1->high_low_container, &x2->high_low_container,
                             pos2, length2, is_cow(x2));
    }
}

roaring_bitmap_t *roaring_bitmap_xor(const roaring_bitmap_t *x1,
                                     const roaring_bitmap_t *x2) {
    uint8_t container_result_type = 0;
    const int length1 = x1->high_low_container.size,
              length2 = x2->high_low_container.size;
    if (0 == length1) {
        return roaring_bitmap_copy(x2);
    }
    if (0 == length2) {
        return roaring_bitmap_copy(x1);
    }
    roaring_bitmap_t *answer =
        roaring_bitmap_create_with_capacity(length1 + length2);
    roaring_bitmap_set_copy_on_write(answer, is_cow(x1) && is_cow(x2));
    int pos1 = 0, pos2 = 0;
    uint8_t container_type_1, container_type_2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
    while (true) {
        if (s1 == s2) {
            void *c1 = ra_get_container_at_index(&x1->high_low_container, pos1,
                                                 &container_type_1);
            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            void *c = container_xor(c1, container_type_1, c2, container_type_2,
                                    &container_result_type);

            if (container_nonzero_cardinality(c, container_result_type)) {
                ra_append(&answer->high_low_container, s1, c,
                          container_result_type);
            } else {
                container_free(c, container_result_type);
            }
            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);

        } else if (s1 < s2) {  // s1 < s2
            void *c1 = ra_get_container_at_index(&x1->high_low_container, pos1,
                                                 &container_type_1);
            c1 =
                get_copy_of_container(c1, &container_type_1, is_cow(x1));
            if (is_cow(x1)) {
                ra_set_container_at_index(&x1->high_low_container, pos1, c1,
                                          container_type_1);
            }
            ra_append(&answer->high_low_container, s1, c1, container_type_1);
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);

        } else {  // s1 > s2
            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            c2 =
                get_copy_of_container(c2, &container_type_2, is_cow(x2));
            if (is_cow(x2)) {
                ra_set_container_at_index(&x2->high_low_container, pos2, c2,
                                          container_type_2);
            }
            ra_append(&answer->high_low_container, s2, c2, container_type_2);
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(&answer->high_low_container,
                             &x2->high_low_container, pos2, length2,
                             is_cow(x2));
    } else if (pos2 == length2) {
        ra_append_copy_range(&answer->high_low_container,
                             &x1->high_low_container, pos1, length1,
                             is_cow(x1));
    }
    return answer;
}

// inplace xor (modifies its first argument).

void roaring_bitmap_xor_inplace(roaring_bitmap_t *x1,
                                const roaring_bitmap_t *x2) {
    assert(x1 != x2);
    uint8_t container_result_type = 0;
    int length1 = x1->high_low_container.size;
    const int length2 = x2->high_low_container.size;

    if (0 == length2) return;

    if (0 == length1) {
        roaring_bitmap_overwrite(x1, x2);
        return;
    }

    // XOR can have new containers inserted from x2, but can also
    // lose containers when x1 and x2 are nonempty and identical.

    int pos1 = 0, pos2 = 0;
    uint8_t container_type_1, container_type_2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
    while (true) {
        if (s1 == s2) {
            void *c1 = ra_get_container_at_index(&x1->high_low_container, pos1,
                                                 &container_type_1);
            c1 = get_writable_copy_if_shared(c1, &container_type_1);

            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            void *c = container_ixor(c1, container_type_1, c2, container_type_2,
                                     &container_result_type);

            if (container_nonzero_cardinality(c, container_result_type)) {
                ra_set_container_at_index(&x1->high_low_container, pos1, c,
                                          container_result_type);
                ++pos1;
            } else {
                container_free(c, container_result_type);
                ra_remove_at_index(&x1->high_low_container, pos1);
                --length1;
            }

            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);

        } else if (s1 < s2) {  // s1 < s2
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);

        } else {  // s1 > s2
            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            c2 =
                get_copy_of_container(c2, &container_type_2, is_cow(x2));
            if (is_cow(x2)) {
                ra_set_container_at_index(&x2->high_low_container, pos2, c2,
                                          container_type_2);
            }

            ra_insert_new_key_value_at(&x1->high_low_container, pos1, s2, c2,
                                       container_type_2);
            pos1++;
            length1++;
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(&x1->high_low_container, &x2->high_low_container,
                             pos2, length2, is_cow(x2));
    }
}

roaring_bitmap_t *roaring_bitmap_andnot(const roaring_bitmap_t *x1,
                                        const roaring_bitmap_t *x2) {
    uint8_t container_result_type = 0;
    const int length1 = x1->high_low_container.size,
              length2 = x2->high_low_container.size;
    if (0 == length1) {
        roaring_bitmap_t *empty_bitmap = roaring_bitmap_create();
        roaring_bitmap_set_copy_on_write(empty_bitmap, is_cow(x1) && is_cow(x2));
        return empty_bitmap;
    }
    if (0 == length2) {
        return roaring_bitmap_copy(x1);
    }
    roaring_bitmap_t *answer = roaring_bitmap_create_with_capacity(length1);
    roaring_bitmap_set_copy_on_write(answer, is_cow(x1) && is_cow(x2));

    int pos1 = 0, pos2 = 0;
    uint8_t container_type_1, container_type_2;
    uint16_t s1 = 0;
    uint16_t s2 = 0;
    while (true) {
        s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
        s2 = ra_get_key_at_index(&x2->high_low_container, pos2);

        if (s1 == s2) {
            void *c1 = ra_get_container_at_index(&x1->high_low_container, pos1,
                                                 &container_type_1);
            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            void *c =
                container_andnot(c1, container_type_1, c2, container_type_2,
                                 &container_result_type);

            if (container_nonzero_cardinality(c, container_result_type)) {
                ra_append(&answer->high_low_container, s1, c,
                          container_result_type);
            } else {
                container_free(c, container_result_type);
            }
            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
        } else if (s1 < s2) {  // s1 < s2
            const int next_pos1 =
                ra_advance_until(&x1->high_low_container, s2, pos1);
            ra_append_copy_range(&answer->high_low_container,
                                 &x1->high_low_container, pos1, next_pos1,
                                 is_cow(x1));
            // TODO : perhaps some of the copy_on_write should be based on
            // answer rather than x1 (more stringent?).  Many similar cases
            pos1 = next_pos1;
            if (pos1 == length1) break;
        } else {  // s1 > s2
            pos2 = ra_advance_until(&x2->high_low_container, s1, pos2);
            if (pos2 == length2) break;
        }
    }
    if (pos2 == length2) {
        ra_append_copy_range(&answer->high_low_container,
                             &x1->high_low_container, pos1, length1,
                             is_cow(x1));
    }
    return answer;
}

// inplace andnot (modifies its first argument).

void roaring_bitmap_andnot_inplace(roaring_bitmap_t *x1,
                                   const roaring_bitmap_t *x2) {
    assert(x1 != x2);

    uint8_t container_result_type = 0;
    int length1 = x1->high_low_container.size;
    const int length2 = x2->high_low_container.size;
    int intersection_size = 0;

    if (0 == length2) return;

    if (0 == length1) {
        roaring_bitmap_clear(x1);
        return;
    }

    int pos1 = 0, pos2 = 0;
    uint8_t container_type_1, container_type_2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
    while (true) {
        if (s1 == s2) {
            void *c1 = ra_get_container_at_index(&x1->high_low_container, pos1,
                                                 &container_type_1);
            c1 = get_writable_copy_if_shared(c1, &container_type_1);

            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            void *c =
                container_iandnot(c1, container_type_1, c2, container_type_2,
                                  &container_result_type);

            if (container_nonzero_cardinality(c, container_result_type)) {
                ra_replace_key_and_container_at_index(&x1->high_low_container,
                                                      intersection_size++, s1,
                                                      c, container_result_type);
            } else {
                container_free(c, container_result_type);
            }

            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);

        } else if (s1 < s2) {  // s1 < s2
            if (pos1 != intersection_size) {
                void *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                     pos1, &container_type_1);

                ra_replace_key_and_container_at_index(&x1->high_low_container,
                                                      intersection_size, s1, c1,
                                                      container_type_1);
            }
            intersection_size++;
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);

        } else {  // s1 > s2
            pos2 = ra_advance_until(&x2->high_low_container, s1, pos2);
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
        }
    }

    if (pos1 < length1) {
        // all containers between intersection_size and
        // pos1 are junk.  However, they have either been moved
        // (thus still referenced) or involved in an iandnot
        // that will clean up all containers that could not be reused.
        // Thus we should not free the junk containers between
        // intersection_size and pos1.
        if (pos1 > intersection_size) {
            // left slide of remaining items
            ra_copy_range(&x1->high_low_container, pos1, length1,
                          intersection_size);
        }
        // else current placement is fine
        intersection_size += (length1 - pos1);
    }
    ra_downsize(&x1->high_low_container, intersection_size);
}

uint64_t roaring_bitmap_get_cardinality(const roaring_bitmap_t *ra) {
    uint64_t card = 0;
    for (int i = 0; i < ra->high_low_container.size; ++i)
        card += container_get_cardinality(ra->high_low_container.containers[i],
                                          ra->high_low_container.typecodes[i]);
    return card;
}

uint64_t roaring_bitmap_range_cardinality(const roaring_bitmap_t *ra,
                                          uint64_t range_start,
                                          uint64_t range_end) {
    if (range_end > UINT32_MAX) {
        range_end = UINT32_MAX + UINT64_C(1);
    }
    if (range_start >= range_end) {
        return 0;
    }
    range_end--; // make range_end inclusive
    // now we have: 0 <= range_start <= range_end <= UINT32_MAX

    int minhb = range_start >> 16;
    int maxhb = range_end >> 16;

    uint64_t card = 0;

    int i = ra_get_index(&ra->high_low_container, minhb);
    if (i >= 0) {
        if (minhb == maxhb) {
            card += container_rank(ra->high_low_container.containers[i],
                                   ra->high_low_container.typecodes[i],
                                   range_end & 0xffff);
        } else {
            card += container_get_cardinality(ra->high_low_container.containers[i],
                                              ra->high_low_container.typecodes[i]);
        }
        if ((range_start & 0xffff) != 0) {
            card -= container_rank(ra->high_low_container.containers[i],
                                   ra->high_low_container.typecodes[i],
                                   (range_start & 0xffff) - 1);
        }
        i++;
    } else {
        i = -i - 1;
    }

    for (; i < ra->high_low_container.size; i++) {
        uint16_t key = ra->high_low_container.keys[i];
        if (key < maxhb) {
            card += container_get_cardinality(ra->high_low_container.containers[i],
                                              ra->high_low_container.typecodes[i]);
        } else if (key == maxhb) {
            card += container_rank(ra->high_low_container.containers[i],
                                   ra->high_low_container.typecodes[i],
                                   range_end & 0xffff);
            break;
        } else {
            break;
        }
    }

    return card;
}


bool roaring_bitmap_is_empty(const roaring_bitmap_t *ra) {
    return ra->high_low_container.size == 0;
}

void roaring_bitmap_to_uint32_array(const roaring_bitmap_t *ra, uint32_t *ans) {
    ra_to_uint32_array(&ra->high_low_container, ans);
}

bool roaring_bitmap_range_uint32_array(const roaring_bitmap_t *ra, size_t offset, size_t limit,  uint32_t *ans) {
    return ra_range_uint32_array(&ra->high_low_container, offset, limit, ans);
}

/** convert array and bitmap containers to run containers when it is more
 * efficient;
 * also convert from run containers when more space efficient.  Returns
 * true if the result has at least one run container.
*/
bool roaring_bitmap_run_optimize(roaring_bitmap_t *r) {
    bool answer = false;
    for (int i = 0; i < r->high_low_container.size; i++) {
        uint8_t typecode_original, typecode_after;
        ra_unshare_container_at_index(
            &r->high_low_container, i);  // TODO: this introduces extra cloning!
        void *c = ra_get_container_at_index(&r->high_low_container, i,
                                            &typecode_original);
        void *c1 = convert_run_optimize(c, typecode_original, &typecode_after);
        if (typecode_after == RUN_CONTAINER_TYPE_CODE) answer = true;
        ra_set_container_at_index(&r->high_low_container, i, c1,
                                  typecode_after);
    }
    return answer;
}

size_t roaring_bitmap_shrink_to_fit(roaring_bitmap_t *r) {
    size_t answer = 0;
    for (int i = 0; i < r->high_low_container.size; i++) {
        uint8_t typecode_original;
        void *c = ra_get_container_at_index(&r->high_low_container, i,
                                            &typecode_original);
        answer += container_shrink_to_fit(c, typecode_original);
    }
    answer += ra_shrink_to_fit(&r->high_low_container);
    return answer;
}

/**
 *  Remove run-length encoding even when it is more space efficient
 *  return whether a change was applied
 */
bool roaring_bitmap_remove_run_compression(roaring_bitmap_t *r) {
    bool answer = false;
    for (int i = 0; i < r->high_low_container.size; i++) {
        uint8_t typecode_original, typecode_after;
        void *c = ra_get_container_at_index(&r->high_low_container, i,
                                            &typecode_original);
        if (get_container_type(c, typecode_original) ==
            RUN_CONTAINER_TYPE_CODE) {
            answer = true;
            if (typecode_original == SHARED_CONTAINER_TYPE_CODE) {
                run_container_t *truec =
                    (run_container_t *)((shared_container_t *)c)->container;
                int32_t card = run_container_cardinality(truec);
                void *c1 = convert_to_bitset_or_array_container(
                    truec, card, &typecode_after);
                shared_container_free((shared_container_t *)c);
                ra_set_container_at_index(&r->high_low_container, i, c1,
                                          typecode_after);

            } else {
                int32_t card = run_container_cardinality((run_container_t *)c);
                void *c1 = convert_to_bitset_or_array_container(
                    (run_container_t *)c, card, &typecode_after);
                ra_set_container_at_index(&r->high_low_container, i, c1,
                                          typecode_after);
            }
        }
    }
    return answer;
}

size_t roaring_bitmap_serialize(const roaring_bitmap_t *ra, char *buf) {
    size_t portablesize = roaring_bitmap_portable_size_in_bytes(ra);
    uint64_t cardinality = roaring_bitmap_get_cardinality(ra);
    uint64_t sizeasarray = cardinality * sizeof(uint32_t) + sizeof(uint32_t);
    if (portablesize < sizeasarray) {
        buf[0] = SERIALIZATION_CONTAINER;
        return roaring_bitmap_portable_serialize(ra, buf + 1) + 1;
    } else {
        buf[0] = SERIALIZATION_ARRAY_UINT32;
        memcpy(buf + 1, &cardinality, sizeof(uint32_t));
        roaring_bitmap_to_uint32_array(
            ra, (uint32_t *)(buf + 1 + sizeof(uint32_t)));
        return 1 + (size_t)sizeasarray;
    }
}

size_t roaring_bitmap_size_in_bytes(const roaring_bitmap_t *ra) {
    size_t portablesize = roaring_bitmap_portable_size_in_bytes(ra);
    uint64_t sizeasarray = roaring_bitmap_get_cardinality(ra) * sizeof(uint32_t) +
                         sizeof(uint32_t);
    return portablesize < sizeasarray ? portablesize + 1 : (size_t)sizeasarray + 1;
}

size_t roaring_bitmap_portable_size_in_bytes(const roaring_bitmap_t *ra) {
    return ra_portable_size_in_bytes(&ra->high_low_container);
}


roaring_bitmap_t *roaring_bitmap_portable_deserialize_safe(const char *buf, size_t maxbytes) {
    roaring_bitmap_t *ans =
        (roaring_bitmap_t *)malloc(sizeof(roaring_bitmap_t));
    if (ans == NULL) {
        return NULL;
    }
    size_t bytesread;
    bool is_ok = ra_portable_deserialize(&ans->high_low_container, buf, maxbytes, &bytesread);
    if(is_ok) assert(bytesread <= maxbytes);
    roaring_bitmap_set_copy_on_write(ans, false);
    if (!is_ok) {
        free(ans);
        return NULL;
    }
    return ans;
}

roaring_bitmap_t *roaring_bitmap_portable_deserialize(const char *buf) {
    return roaring_bitmap_portable_deserialize_safe(buf, SIZE_MAX);
}


size_t roaring_bitmap_portable_deserialize_size(const char *buf, size_t maxbytes) {
  return ra_portable_deserialize_size(buf, maxbytes);
}


size_t roaring_bitmap_portable_serialize(const roaring_bitmap_t *ra,
                                         char *buf) {
    return ra_portable_serialize(&ra->high_low_container, buf);
}

roaring_bitmap_t *roaring_bitmap_deserialize(const void *buf) {
    const char *bufaschar = (const char *)buf;
    if (*(const unsigned char *)buf == SERIALIZATION_ARRAY_UINT32) {
        /* This looks like a compressed set of uint32_t elements */
        uint32_t card;
        memcpy(&card, bufaschar + 1, sizeof(uint32_t));
        const uint32_t *elems =
            (const uint32_t *)(bufaschar + 1 + sizeof(uint32_t));

        return roaring_bitmap_of_ptr(card, elems);
    } else if (bufaschar[0] == SERIALIZATION_CONTAINER) {
        return roaring_bitmap_portable_deserialize(bufaschar + 1);
    } else
        return (NULL);
}

bool roaring_iterate(const roaring_bitmap_t *ra, roaring_iterator iterator,
                     void *ptr) {
    for (int i = 0; i < ra->high_low_container.size; ++i)
        if (!container_iterate(ra->high_low_container.containers[i],
                               ra->high_low_container.typecodes[i],
                               ((uint32_t)ra->high_low_container.keys[i]) << 16,
                               iterator, ptr)) {
            return false;
        }
    return true;
}

bool roaring_iterate64(const roaring_bitmap_t *ra, roaring_iterator64 iterator,
                       uint64_t high_bits, void *ptr) {
    for (int i = 0; i < ra->high_low_container.size; ++i)
        if (!container_iterate64(
                ra->high_low_container.containers[i],
                ra->high_low_container.typecodes[i],
                ((uint32_t)ra->high_low_container.keys[i]) << 16, iterator,
                high_bits, ptr)) {
            return false;
        }
    return true;
}

/****
* begin roaring_uint32_iterator_t
*****/

// Partially initializes the roaring iterator when it begins looking at
// a new container.
static bool iter_new_container_partial_init(roaring_uint32_iterator_t *newit) {
    newit->in_container_index = 0;
    newit->run_index = 0;
    newit->current_value = 0;
    if (newit->container_index >= newit->parent->high_low_container.size ||
        newit->container_index < 0) {
        newit->current_value = UINT32_MAX;
        return (newit->has_value = false);
    }
    // assume not empty
    newit->has_value = true;
    // we precompute container, typecode and highbits so that successive
    // iterators do not have to grab them from odd memory locations
    // and have to worry about the (easily predicted) container_unwrap_shared
    // call.
    newit->container =
            newit->parent->high_low_container.containers[newit->container_index];
    newit->typecode =
            newit->parent->high_low_container.typecodes[newit->container_index];
    newit->highbits =
            ((uint32_t)
                    newit->parent->high_low_container.keys[newit->container_index])
                    << 16;
    newit->container =
            container_unwrap_shared(newit->container, &(newit->typecode));
    return newit->has_value;
}

static bool loadfirstvalue(roaring_uint32_iterator_t *newit) {
    if (!iter_new_container_partial_init(newit))
        return newit->has_value;

    uint32_t wordindex;
    uint64_t word;  // used for bitsets
    switch (newit->typecode) {
        case BITSET_CONTAINER_TYPE_CODE:
            wordindex = 0;
            while ((word = ((const bitset_container_t *)(newit->container))
                               ->array[wordindex]) == 0)
                wordindex++;  // advance
            // here "word" is non-zero
            newit->in_container_index = wordindex * 64 + __builtin_ctzll(word);
            newit->current_value = newit->highbits | newit->in_container_index;
            break;
        case ARRAY_CONTAINER_TYPE_CODE:
            newit->current_value =
                newit->highbits |
                ((const array_container_t *)(newit->container))->array[0];
            break;
        case RUN_CONTAINER_TYPE_CODE:
            newit->current_value =
                newit->highbits |
                (((const run_container_t *)(newit->container))->runs[0].value);
            break;
        default:
            // if this ever happens, bug!
            assert(false);
    }  // switch (typecode)
    return true;
}

static bool loadlastvalue(roaring_uint32_iterator_t* newit) {
    if (!iter_new_container_partial_init(newit))
        return newit->has_value;

    switch(newit->typecode) {
        case BITSET_CONTAINER_TYPE_CODE: {
            uint32_t wordindex = BITSET_CONTAINER_SIZE_IN_WORDS - 1;
            uint64_t word;
            const bitset_container_t* bitset_container = (const bitset_container_t*)newit->container;
            while ((word = bitset_container->array[wordindex]) == 0)
                --wordindex;

            int num_leading_zeros = __builtin_clzll(word);
            newit->in_container_index = (wordindex * 64) + (63 - num_leading_zeros);
            newit->current_value = newit->highbits | newit->in_container_index;
            break;
        }
        case ARRAY_CONTAINER_TYPE_CODE: {
            const array_container_t* array_container = (const array_container_t*)newit->container;
            newit->in_container_index = array_container->cardinality - 1;
            newit->current_value = newit->highbits | array_container->array[newit->in_container_index];
            break;
        }
        case RUN_CONTAINER_TYPE_CODE: {
            const run_container_t* run_container = (const run_container_t*)newit->container;
            newit->run_index = run_container->n_runs - 1;
            const rle16_t* last_run = &run_container->runs[newit->run_index];
            newit->current_value = newit->highbits | (last_run->value + last_run->length);
            break;
        }
        default:
            // if this ever happens, bug!
            assert(false);
    }
    return true;
}

// prerequesite: the value should be in range of the container
static bool loadfirstvalue_largeorequal(roaring_uint32_iterator_t *newit, uint32_t val) {
    // Don't have to check return value because of prerequisite
    iter_new_container_partial_init(newit);
    uint16_t lb = val & 0xFFFF;

    switch (newit->typecode) {
        case BITSET_CONTAINER_TYPE_CODE:
            newit->in_container_index =  bitset_container_index_equalorlarger((const bitset_container_t *)(newit->container), lb);
            newit->current_value = newit->highbits | newit->in_container_index;
            break;
        case ARRAY_CONTAINER_TYPE_CODE:
            newit->in_container_index = array_container_index_equalorlarger((const array_container_t *)(newit->container), lb);
            newit->current_value =
                newit->highbits |
                ((const array_container_t *)(newit->container))->array[newit->in_container_index];
            break;
        case RUN_CONTAINER_TYPE_CODE:
            newit->run_index = run_container_index_equalorlarger((const run_container_t *)(newit->container), lb);
            if(((const run_container_t *)(newit->container))->runs[newit->run_index].value <= lb) {
              newit->current_value = val;
            } else {
              newit->current_value =
                newit->highbits |
                (((const run_container_t *)(newit->container))->runs[newit->run_index].value);
            }
            break;
        default:
            // if this ever happens, bug!
            assert(false);
    }  // switch (typecode)
    return true;
}

void roaring_init_iterator(const roaring_bitmap_t *ra,
                           roaring_uint32_iterator_t *newit) {
    newit->parent = ra;
    newit->container_index = 0;
    newit->has_value = loadfirstvalue(newit);
}

void roaring_init_iterator_last(const roaring_bitmap_t *ra,
                                roaring_uint32_iterator_t *newit) {
    newit->parent = ra;
    newit->container_index = newit->parent->high_low_container.size - 1;
    newit->has_value = loadlastvalue(newit);
}

roaring_uint32_iterator_t *roaring_create_iterator(const roaring_bitmap_t *ra) {
    roaring_uint32_iterator_t *newit =
        (roaring_uint32_iterator_t *)malloc(sizeof(roaring_uint32_iterator_t));
    if (newit == NULL) return NULL;
    roaring_init_iterator(ra, newit);
    return newit;
}

roaring_uint32_iterator_t *roaring_copy_uint32_iterator(
    const roaring_uint32_iterator_t *it) {
    roaring_uint32_iterator_t *newit =
        (roaring_uint32_iterator_t *)malloc(sizeof(roaring_uint32_iterator_t));
    memcpy(newit, it, sizeof(roaring_uint32_iterator_t));
    return newit;
}

bool roaring_move_uint32_iterator_equalorlarger(roaring_uint32_iterator_t *it, uint32_t val) {
    uint16_t hb = val >> 16;
    const int i = ra_get_index(& it->parent->high_low_container, hb);
    if (i >= 0) {
      uint32_t lowvalue = container_maximum(it->parent->high_low_container.containers[i], it->parent->high_low_container.typecodes[i]);
      uint16_t lb = val & 0xFFFF;
      if(lowvalue < lb ) {
        it->container_index = i+1; // will have to load first value of next container
      } else {// the value is necessarily within the range of the container
        it->container_index = i;
        it->has_value = loadfirstvalue_largeorequal(it, val);
        return it->has_value;
      }
    } else {
      // there is no matching, so we are going for the next container
      it->container_index = -i-1;
    }
    it->has_value = loadfirstvalue(it);
    return it->has_value;
}


bool roaring_advance_uint32_iterator(roaring_uint32_iterator_t *it) {
    if (it->container_index >= it->parent->high_low_container.size) {
        return (it->has_value = false);
    }
    if (it->container_index < 0) {
        it->container_index = 0;
        return (it->has_value = loadfirstvalue(it));
    }

    uint32_t wordindex;  // used for bitsets
    uint64_t word;       // used for bitsets
    switch (it->typecode) {
        case BITSET_CONTAINER_TYPE_CODE:
            it->in_container_index++;
            wordindex = it->in_container_index / 64;
            if (wordindex >= BITSET_CONTAINER_SIZE_IN_WORDS) break;
            word = ((const bitset_container_t *)(it->container))
                       ->array[wordindex] &
                   (UINT64_MAX << (it->in_container_index % 64));
            // next part could be optimized/simplified
            while ((word == 0) &&
                   (wordindex + 1 < BITSET_CONTAINER_SIZE_IN_WORDS)) {
                wordindex++;
                word = ((const bitset_container_t *)(it->container))
                           ->array[wordindex];
            }
            if (word != 0) {
                it->in_container_index = wordindex * 64 + __builtin_ctzll(word);
                it->current_value = it->highbits | it->in_container_index;
                return (it->has_value = true);
            }
            break;
        case ARRAY_CONTAINER_TYPE_CODE:
            it->in_container_index++;
            if (it->in_container_index <
                ((const array_container_t *)(it->container))->cardinality) {
                it->current_value = it->highbits |
                                    ((const array_container_t *)(it->container))
                                        ->array[it->in_container_index];
                return (it->has_value = true);
            }
            break;
        case RUN_CONTAINER_TYPE_CODE: {
            if(it->current_value == UINT32_MAX) {
                return (it->has_value = false); // without this, we risk an overflow to zero
            }

            const run_container_t* run_container = (const run_container_t*)it->container;
            if (++it->current_value <= (it->highbits | (run_container->runs[it->run_index].value +
                                                        run_container->runs[it->run_index].length))) {
                return (it->has_value = true);
            }

            if (++it->run_index < run_container->n_runs) {
                // Assume the run has a value
                it->current_value = it->highbits | run_container->runs[it->run_index].value;
                return (it->has_value = true);
            }
            break;
        }
        default:
            // if this ever happens, bug!
            assert(false);
    }  // switch (typecode)
    // moving to next container
    it->container_index++;
    return (it->has_value = loadfirstvalue(it));
}

bool roaring_previous_uint32_iterator(roaring_uint32_iterator_t *it) {
    if (it->container_index < 0) {
        return (it->has_value = false);
    }
    if (it->container_index >= it->parent->high_low_container.size) {
        it->container_index = it->parent->high_low_container.size - 1;
        return (it->has_value = loadlastvalue(it));
    }

    switch (it->typecode) {
        case BITSET_CONTAINER_TYPE_CODE: {
            if (--it->in_container_index < 0)
                break;

            const bitset_container_t* bitset_container = (const bitset_container_t*)it->container;
            int32_t wordindex = it->in_container_index / 64;
            uint64_t word = bitset_container->array[wordindex] & (UINT64_MAX >> (63 - (it->in_container_index % 64)));

            while (word == 0 && --wordindex >= 0) {
                word = bitset_container->array[wordindex];
            }
            if (word == 0)
                break;

            int num_leading_zeros = __builtin_clzll(word);
            it->in_container_index = (wordindex * 64) + (63 - num_leading_zeros);
            it->current_value = it->highbits | it->in_container_index;
            return (it->has_value = true);
        }
        case ARRAY_CONTAINER_TYPE_CODE: {
            if (--it->in_container_index < 0)
                break;

            const array_container_t* array_container = (const array_container_t*)it->container;
            it->current_value = it->highbits | array_container->array[it->in_container_index];
            return (it->has_value = true);
        }
        case RUN_CONTAINER_TYPE_CODE: {
            if(it->current_value == 0)
                return (it->has_value = false);

            const run_container_t* run_container = (const run_container_t*)it->container;
            if (--it->current_value >= (it->highbits | run_container->runs[it->run_index].value)) {
                return (it->has_value = true);
            }

            if (--it->run_index < 0)
                break;

            it->current_value = it->highbits | (run_container->runs[it->run_index].value +
                                                run_container->runs[it->run_index].length);
            return (it->has_value = true);
        }
        default:
            // if this ever happens, bug!
            assert(false);
    }  // switch (typecode)

    // moving to previous container
    it->container_index--;
    return (it->has_value = loadlastvalue(it));
}

uint32_t roaring_read_uint32_iterator(roaring_uint32_iterator_t *it, uint32_t* buf, uint32_t count) {
  uint32_t ret = 0;
  uint32_t num_values;
  uint32_t wordindex;  // used for bitsets
  uint64_t word;       // used for bitsets
  const array_container_t* acont; //TODO remove
  const run_container_t* rcont; //TODO remove
  const bitset_container_t* bcont; //TODO remove

  while (it->has_value && ret < count) {
    switch (it->typecode) {
      case BITSET_CONTAINER_TYPE_CODE:
        bcont = (const bitset_container_t*)(it->container);
        wordindex = it->in_container_index / 64;
        word = bcont->array[wordindex] & (UINT64_MAX << (it->in_container_index % 64));
        do {
          while (word != 0 && ret < count) {
            buf[0] = it->highbits | (wordindex * 64 + __builtin_ctzll(word));
            word = word & (word - 1);
            buf++;
            ret++;
          }
          while (word == 0 && wordindex+1 < BITSET_CONTAINER_SIZE_IN_WORDS) {
            wordindex++;
            word = bcont->array[wordindex];
          }
        } while (word != 0 && ret < count);
        it->has_value = (word != 0);
        if (it->has_value) {
          it->in_container_index = wordindex * 64 + __builtin_ctzll(word);
          it->current_value = it->highbits | it->in_container_index;
        }
        break;
      case ARRAY_CONTAINER_TYPE_CODE:
        acont = (const array_container_t *)(it->container);
        num_values = minimum_uint32(acont->cardinality - it->in_container_index, count - ret);
        for (uint32_t i = 0; i < num_values; i++) {
          buf[i] = it->highbits | acont->array[it->in_container_index + i];
        }
        buf += num_values;
        ret += num_values;
        it->in_container_index += num_values;
        it->has_value = (it->in_container_index < acont->cardinality);
        if (it->has_value) {
          it->current_value = it->highbits | acont->array[it->in_container_index];
        }
        break;
      case RUN_CONTAINER_TYPE_CODE:
        rcont = (const run_container_t*)(it->container);
        //"in_run_index" name is misleading, read it as "max_value_in_current_run"
        do {
          uint32_t largest_run_value = it->highbits | (rcont->runs[it->run_index].value + rcont->runs[it->run_index].length);
          num_values = minimum_uint32(largest_run_value - it->current_value + 1, count - ret);
          for (uint32_t i = 0; i < num_values; i++) {
            buf[i] = it->current_value + i;
          }
          it->current_value += num_values; // this can overflow to zero: UINT32_MAX+1=0
          buf += num_values;
          ret += num_values;

          if (it->current_value > largest_run_value || it->current_value == 0) {
            it->run_index++;
            if (it->run_index < rcont->n_runs) {
              it->current_value = it->highbits | rcont->runs[it->run_index].value;
            } else {
              it->has_value = false;
            }
          }
        } while ((ret < count) && it->has_value);
        break;
      default:
        assert(false);
    }
    if (it->has_value) {
      assert(ret == count);
      return ret;
    }
    it->container_index++;
    it->has_value = loadfirstvalue(it);
  }
  return ret;
}



void roaring_free_uint32_iterator(roaring_uint32_iterator_t *it) { free(it); }

/****
* end of roaring_uint32_iterator_t
*****/

bool roaring_bitmap_equals(const roaring_bitmap_t *ra1,
                           const roaring_bitmap_t *ra2) {
    if (ra1->high_low_container.size != ra2->high_low_container.size) {
        return false;
    }
    for (int i = 0; i < ra1->high_low_container.size; ++i) {
        if (ra1->high_low_container.keys[i] !=
            ra2->high_low_container.keys[i]) {
            return false;
        }
    }
    for (int i = 0; i < ra1->high_low_container.size; ++i) {
        bool areequal = container_equals(ra1->high_low_container.containers[i],
                                         ra1->high_low_container.typecodes[i],
                                         ra2->high_low_container.containers[i],
                                         ra2->high_low_container.typecodes[i]);
        if (!areequal) {
            return false;
        }
    }
    return true;
}

bool roaring_bitmap_is_subset(const roaring_bitmap_t *ra1,
                              const roaring_bitmap_t *ra2) {
    const int length1 = ra1->high_low_container.size,
              length2 = ra2->high_low_container.size;

    int pos1 = 0, pos2 = 0;

    while (pos1 < length1 && pos2 < length2) {
        const uint16_t s1 = ra_get_key_at_index(&ra1->high_low_container, pos1);
        const uint16_t s2 = ra_get_key_at_index(&ra2->high_low_container, pos2);

        if (s1 == s2) {
            uint8_t container_type_1, container_type_2;
            void *c1 = ra_get_container_at_index(&ra1->high_low_container, pos1,
                                                 &container_type_1);
            void *c2 = ra_get_container_at_index(&ra2->high_low_container, pos2,
                                                 &container_type_2);
            bool subset =
                container_is_subset(c1, container_type_1, c2, container_type_2);
            if (!subset) return false;
            ++pos1;
            ++pos2;
        } else if (s1 < s2) {  // s1 < s2
            return false;
        } else {  // s1 > s2
            pos2 = ra_advance_until(&ra2->high_low_container, s1, pos2);
        }
    }
    if (pos1 == length1)
        return true;
    else
        return false;
}

static void insert_flipped_container(roaring_array_t *ans_arr,
                                     const roaring_array_t *x1_arr, uint16_t hb,
                                     uint16_t lb_start, uint16_t lb_end) {
    const int i = ra_get_index(x1_arr, hb);
    const int j = ra_get_index(ans_arr, hb);
    uint8_t ctype_in, ctype_out;
    void *flipped_container = NULL;
    if (i >= 0) {
        void *container_to_flip =
            ra_get_container_at_index(x1_arr, i, &ctype_in);
        flipped_container =
            container_not_range(container_to_flip, ctype_in, (uint32_t)lb_start,
                                (uint32_t)(lb_end + 1), &ctype_out);

        if (container_get_cardinality(flipped_container, ctype_out))
            ra_insert_new_key_value_at(ans_arr, -j - 1, hb, flipped_container,
                                       ctype_out);
        else {
            container_free(flipped_container, ctype_out);
        }
    } else {
        flipped_container = container_range_of_ones(
            (uint32_t)lb_start, (uint32_t)(lb_end + 1), &ctype_out);
        ra_insert_new_key_value_at(ans_arr, -j - 1, hb, flipped_container,
                                   ctype_out);
    }
}

static void inplace_flip_container(roaring_array_t *x1_arr, uint16_t hb,
                                   uint16_t lb_start, uint16_t lb_end) {
    const int i = ra_get_index(x1_arr, hb);
    uint8_t ctype_in, ctype_out;
    void *flipped_container = NULL;
    if (i >= 0) {
        void *container_to_flip =
            ra_get_container_at_index(x1_arr, i, &ctype_in);
        flipped_container = container_inot_range(
            container_to_flip, ctype_in, (uint32_t)lb_start,
            (uint32_t)(lb_end + 1), &ctype_out);
        // if a new container was created, the old one was already freed
        if (container_get_cardinality(flipped_container, ctype_out)) {
            ra_set_container_at_index(x1_arr, i, flipped_container, ctype_out);
        } else {
            container_free(flipped_container, ctype_out);
            ra_remove_at_index(x1_arr, i);
        }

    } else {
        flipped_container = container_range_of_ones(
            (uint32_t)lb_start, (uint32_t)(lb_end + 1), &ctype_out);
        ra_insert_new_key_value_at(x1_arr, -i - 1, hb, flipped_container,
                                   ctype_out);
    }
}

static void insert_fully_flipped_container(roaring_array_t *ans_arr,
                                           const roaring_array_t *x1_arr,
                                           uint16_t hb) {
    const int i = ra_get_index(x1_arr, hb);
    const int j = ra_get_index(ans_arr, hb);
    uint8_t ctype_in, ctype_out;
    void *flipped_container = NULL;
    if (i >= 0) {
        void *container_to_flip =
            ra_get_container_at_index(x1_arr, i, &ctype_in);
        flipped_container =
            container_not(container_to_flip, ctype_in, &ctype_out);
        if (container_get_cardinality(flipped_container, ctype_out))
            ra_insert_new_key_value_at(ans_arr, -j - 1, hb, flipped_container,
                                       ctype_out);
        else {
            container_free(flipped_container, ctype_out);
        }
    } else {
        flipped_container = container_range_of_ones(0U, 0x10000U, &ctype_out);
        ra_insert_new_key_value_at(ans_arr, -j - 1, hb, flipped_container,
                                   ctype_out);
    }
}

static void inplace_fully_flip_container(roaring_array_t *x1_arr, uint16_t hb) {
    const int i = ra_get_index(x1_arr, hb);
    uint8_t ctype_in, ctype_out;
    void *flipped_container = NULL;
    if (i >= 0) {
        void *container_to_flip =
            ra_get_container_at_index(x1_arr, i, &ctype_in);
        flipped_container =
            container_inot(container_to_flip, ctype_in, &ctype_out);

        if (container_get_cardinality(flipped_container, ctype_out)) {
            ra_set_container_at_index(x1_arr, i, flipped_container, ctype_out);
        } else {
            container_free(flipped_container, ctype_out);
            ra_remove_at_index(x1_arr, i);
        }

    } else {
        flipped_container = container_range_of_ones(0U, 0x10000U, &ctype_out);
        ra_insert_new_key_value_at(x1_arr, -i - 1, hb, flipped_container,
                                   ctype_out);
    }
}

roaring_bitmap_t *roaring_bitmap_flip(const roaring_bitmap_t *x1,
                                      uint64_t range_start,
                                      uint64_t range_end) {
    if (range_start >= range_end) {
        return roaring_bitmap_copy(x1);
    }
    if(range_end >= UINT64_C(0x100000000)) {
        range_end = UINT64_C(0x100000000);
    }

    roaring_bitmap_t *ans = roaring_bitmap_create();
    roaring_bitmap_set_copy_on_write(ans, is_cow(x1));

    uint16_t hb_start = (uint16_t)(range_start >> 16);
    const uint16_t lb_start = (uint16_t)range_start;  // & 0xFFFF;
    uint16_t hb_end = (uint16_t)((range_end - 1) >> 16);
    const uint16_t lb_end = (uint16_t)(range_end - 1);  // & 0xFFFF;

    ra_append_copies_until(&ans->high_low_container, &x1->high_low_container,
                           hb_start, is_cow(x1));
    if (hb_start == hb_end) {
        insert_flipped_container(&ans->high_low_container,
                                 &x1->high_low_container, hb_start, lb_start,
                                 lb_end);
    } else {
        // start and end containers are distinct
        if (lb_start > 0) {
            // handle first (partial) container
            insert_flipped_container(&ans->high_low_container,
                                     &x1->high_low_container, hb_start,
                                     lb_start, 0xFFFF);
            ++hb_start;  // for the full containers.  Can't wrap.
        }

        if (lb_end != 0xFFFF) --hb_end;  // later we'll handle the partial block

        for (uint32_t hb = hb_start; hb <= hb_end; ++hb) {
            insert_fully_flipped_container(&ans->high_low_container,
                                           &x1->high_low_container, hb);
        }

        // handle a partial final container
        if (lb_end != 0xFFFF) {
            insert_flipped_container(&ans->high_low_container,
                                     &x1->high_low_container, hb_end + 1, 0,
                                     lb_end);
            ++hb_end;
        }
    }
    ra_append_copies_after(&ans->high_low_container, &x1->high_low_container,
                           hb_end, is_cow(x1));
    return ans;
}

void roaring_bitmap_flip_inplace(roaring_bitmap_t *x1, uint64_t range_start,
                                 uint64_t range_end) {
    if (range_start >= range_end) {
        return;  // empty range
    }
    if(range_end >= UINT64_C(0x100000000)) {
        range_end = UINT64_C(0x100000000);
    }

    uint16_t hb_start = (uint16_t)(range_start >> 16);
    const uint16_t lb_start = (uint16_t)range_start;
    uint16_t hb_end = (uint16_t)((range_end - 1) >> 16);
    const uint16_t lb_end = (uint16_t)(range_end - 1);

    if (hb_start == hb_end) {
        inplace_flip_container(&x1->high_low_container, hb_start, lb_start,
                               lb_end);
    } else {
        // start and end containers are distinct
        if (lb_start > 0) {
            // handle first (partial) container
            inplace_flip_container(&x1->high_low_container, hb_start, lb_start,
                                   0xFFFF);
            ++hb_start;  // for the full containers.  Can't wrap.
        }

        if (lb_end != 0xFFFF) --hb_end;

        for (uint32_t hb = hb_start; hb <= hb_end; ++hb) {
            inplace_fully_flip_container(&x1->high_low_container, hb);
        }
        // handle a partial final container
        if (lb_end != 0xFFFF) {
            inplace_flip_container(&x1->high_low_container, hb_end + 1, 0,
                                   lb_end);
            ++hb_end;
        }
    }
}

roaring_bitmap_t *roaring_bitmap_lazy_or(const roaring_bitmap_t *x1,
                                         const roaring_bitmap_t *x2,
                                         const bool bitsetconversion) {
    uint8_t container_result_type = 0;
    const int length1 = x1->high_low_container.size,
              length2 = x2->high_low_container.size;
    if (0 == length1) {
        return roaring_bitmap_copy(x2);
    }
    if (0 == length2) {
        return roaring_bitmap_copy(x1);
    }
    roaring_bitmap_t *answer =
        roaring_bitmap_create_with_capacity(length1 + length2);
    roaring_bitmap_set_copy_on_write(answer, is_cow(x1) && is_cow(x2));
    int pos1 = 0, pos2 = 0;
    uint8_t container_type_1, container_type_2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
    while (true) {
        if (s1 == s2) {
            void *c1 = ra_get_container_at_index(&x1->high_low_container, pos1,
                                                 &container_type_1);
            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            void *c;
            if (bitsetconversion && (get_container_type(c1, container_type_1) !=
                                     BITSET_CONTAINER_TYPE_CODE) &&
                (get_container_type(c2, container_type_2) !=
                 BITSET_CONTAINER_TYPE_CODE)) {
                void *newc1 =
                    container_mutable_unwrap_shared(c1, &container_type_1);
                newc1 = container_to_bitset(newc1, container_type_1);
                container_type_1 = BITSET_CONTAINER_TYPE_CODE;
                c = container_lazy_ior(newc1, container_type_1, c2,
                                       container_type_2,
                                       &container_result_type);
                if (c != newc1) {  // should not happen
                    container_free(newc1, container_type_1);
                }
            } else {
                c = container_lazy_or(c1, container_type_1, c2,
                                      container_type_2, &container_result_type);
            }
            // since we assume that the initial containers are non-empty,
            // the
            // result here
            // can only be non-empty
            ra_append(&answer->high_low_container, s1, c,
                      container_result_type);
            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);

        } else if (s1 < s2) {  // s1 < s2
            void *c1 = ra_get_container_at_index(&x1->high_low_container, pos1,
                                                 &container_type_1);
            c1 =
                get_copy_of_container(c1, &container_type_1, is_cow(x1));
            if (is_cow(x1)) {
                ra_set_container_at_index(&x1->high_low_container, pos1, c1,
                                          container_type_1);
            }
            ra_append(&answer->high_low_container, s1, c1, container_type_1);
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);

        } else {  // s1 > s2
            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            c2 =
                get_copy_of_container(c2, &container_type_2, is_cow(x2));
            if (is_cow(x2)) {
                ra_set_container_at_index(&x2->high_low_container, pos2, c2,
                                          container_type_2);
            }
            ra_append(&answer->high_low_container, s2, c2, container_type_2);
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(&answer->high_low_container,
                             &x2->high_low_container, pos2, length2,
                             is_cow(x2));
    } else if (pos2 == length2) {
        ra_append_copy_range(&answer->high_low_container,
                             &x1->high_low_container, pos1, length1,
                             is_cow(x1));
    }
    return answer;
}

void roaring_bitmap_lazy_or_inplace(roaring_bitmap_t *x1,
                                    const roaring_bitmap_t *x2,
                                    const bool bitsetconversion) {
    uint8_t container_result_type = 0;
    int length1 = x1->high_low_container.size;
    const int length2 = x2->high_low_container.size;

    if (0 == length2) return;

    if (0 == length1) {
        roaring_bitmap_overwrite(x1, x2);
        return;
    }
    int pos1 = 0, pos2 = 0;
    uint8_t container_type_1, container_type_2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
    while (true) {
        if (s1 == s2) {
            void *c1 = ra_get_container_at_index(&x1->high_low_container, pos1,
                                                 &container_type_1);
            if (!container_is_full(c1, container_type_1)) {
                if ((bitsetconversion == false) ||
                    (get_container_type(c1, container_type_1) ==
                     BITSET_CONTAINER_TYPE_CODE)) {
                    c1 = get_writable_copy_if_shared(c1, &container_type_1);
                } else {
                    // convert to bitset
                    void *oldc1 = c1;
                    uint8_t oldt1 = container_type_1;
                    c1 = container_mutable_unwrap_shared(c1, &container_type_1);
                    c1 = container_to_bitset(c1, container_type_1);
                    container_free(oldc1, oldt1);
                    container_type_1 = BITSET_CONTAINER_TYPE_CODE;
                }

                void *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                     pos2, &container_type_2);
                void *c = container_lazy_ior(c1, container_type_1, c2,
                                             container_type_2,
                                             &container_result_type);
                if (c !=
                    c1) {  // in this instance a new container was created, and
                           // we need to free the old one
                    container_free(c1, container_type_1);
                }

                ra_set_container_at_index(&x1->high_low_container, pos1, c,
                                          container_result_type);
            }
            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);

        } else if (s1 < s2) {  // s1 < s2
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);

        } else {  // s1 > s2
            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            // void *c2_clone = container_clone(c2, container_type_2);
            c2 =
                get_copy_of_container(c2, &container_type_2, is_cow(x2));
            if (is_cow(x2)) {
                ra_set_container_at_index(&x2->high_low_container, pos2, c2,
                                          container_type_2);
            }
            ra_insert_new_key_value_at(&x1->high_low_container, pos1, s2, c2,
                                       container_type_2);
            pos1++;
            length1++;
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(&x1->high_low_container, &x2->high_low_container,
                             pos2, length2, is_cow(x2));
    }
}

roaring_bitmap_t *roaring_bitmap_lazy_xor(const roaring_bitmap_t *x1,
                                          const roaring_bitmap_t *x2) {
    uint8_t container_result_type = 0;
    const int length1 = x1->high_low_container.size,
              length2 = x2->high_low_container.size;
    if (0 == length1) {
        return roaring_bitmap_copy(x2);
    }
    if (0 == length2) {
        return roaring_bitmap_copy(x1);
    }
    roaring_bitmap_t *answer =
        roaring_bitmap_create_with_capacity(length1 + length2);
    roaring_bitmap_set_copy_on_write(answer, is_cow(x1) && is_cow(x2));
    int pos1 = 0, pos2 = 0;
    uint8_t container_type_1, container_type_2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
    while (true) {
        if (s1 == s2) {
            void *c1 = ra_get_container_at_index(&x1->high_low_container, pos1,
                                                 &container_type_1);
            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            void *c =
                container_lazy_xor(c1, container_type_1, c2, container_type_2,
                                   &container_result_type);

            if (container_nonzero_cardinality(c, container_result_type)) {
                ra_append(&answer->high_low_container, s1, c,
                          container_result_type);
            } else {
                container_free(c, container_result_type);
            }

            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);

        } else if (s1 < s2) {  // s1 < s2
            void *c1 = ra_get_container_at_index(&x1->high_low_container, pos1,
                                                 &container_type_1);
            c1 =
                get_copy_of_container(c1, &container_type_1, is_cow(x1));
            if (is_cow(x1)) {
                ra_set_container_at_index(&x1->high_low_container, pos1, c1,
                                          container_type_1);
            }
            ra_append(&answer->high_low_container, s1, c1, container_type_1);
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);

        } else {  // s1 > s2
            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            c2 =
                get_copy_of_container(c2, &container_type_2, is_cow(x2));
            if (is_cow(x2)) {
                ra_set_container_at_index(&x2->high_low_container, pos2, c2,
                                          container_type_2);
            }
            ra_append(&answer->high_low_container, s2, c2, container_type_2);
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(&answer->high_low_container,
                             &x2->high_low_container, pos2, length2,
                             is_cow(x2));
    } else if (pos2 == length2) {
        ra_append_copy_range(&answer->high_low_container,
                             &x1->high_low_container, pos1, length1,
                             is_cow(x1));
    }
    return answer;
}

void roaring_bitmap_lazy_xor_inplace(roaring_bitmap_t *x1,
                                     const roaring_bitmap_t *x2) {
    assert(x1 != x2);
    uint8_t container_result_type = 0;
    int length1 = x1->high_low_container.size;
    const int length2 = x2->high_low_container.size;

    if (0 == length2) return;

    if (0 == length1) {
        roaring_bitmap_overwrite(x1, x2);
        return;
    }
    int pos1 = 0, pos2 = 0;
    uint8_t container_type_1, container_type_2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
    while (true) {
        if (s1 == s2) {
            void *c1 = ra_get_container_at_index(&x1->high_low_container, pos1,
                                                 &container_type_1);
            c1 = get_writable_copy_if_shared(c1, &container_type_1);
            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            void *c =
                container_lazy_ixor(c1, container_type_1, c2, container_type_2,
                                    &container_result_type);
            if (container_nonzero_cardinality(c, container_result_type)) {
                ra_set_container_at_index(&x1->high_low_container, pos1, c,
                                          container_result_type);
                ++pos1;
            } else {
                container_free(c, container_result_type);
                ra_remove_at_index(&x1->high_low_container, pos1);
                --length1;
            }
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);

        } else if (s1 < s2) {  // s1 < s2
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, pos1);

        } else {  // s1 > s2
            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            // void *c2_clone = container_clone(c2, container_type_2);
            c2 =
                get_copy_of_container(c2, &container_type_2, is_cow(x2));
            if (is_cow(x2)) {
                ra_set_container_at_index(&x2->high_low_container, pos2, c2,
                                          container_type_2);
            }
            ra_insert_new_key_value_at(&x1->high_low_container, pos1, s2, c2,
                                       container_type_2);
            pos1++;
            length1++;
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(&x1->high_low_container, &x2->high_low_container,
                             pos2, length2, is_cow(x2));
    }
}

void roaring_bitmap_repair_after_lazy(roaring_bitmap_t *ra) {
    for (int i = 0; i < ra->high_low_container.size; ++i) {
        const uint8_t original_typecode = ra->high_low_container.typecodes[i];
        void *container = ra->high_low_container.containers[i];
        uint8_t new_typecode = original_typecode;
        void *newcontainer =
            container_repair_after_lazy(container, &new_typecode);
        ra->high_low_container.containers[i] = newcontainer;
        ra->high_low_container.typecodes[i] = new_typecode;
    }
}



/**
* roaring_bitmap_rank returns the number of integers that are smaller or equal
* to x.
*/
uint64_t roaring_bitmap_rank(const roaring_bitmap_t *bm, uint32_t x) {
    uint64_t size = 0;
    uint32_t xhigh = x >> 16;
    for (int i = 0; i < bm->high_low_container.size; i++) {
        uint32_t key = bm->high_low_container.keys[i];
        if (xhigh > key) {
            size +=
                container_get_cardinality(bm->high_low_container.containers[i],
                                          bm->high_low_container.typecodes[i]);
        } else if (xhigh == key) {
            return size + container_rank(bm->high_low_container.containers[i],
                                         bm->high_low_container.typecodes[i],
                                         x & 0xFFFF);
        } else {
            return size;
        }
    }
    return size;
}

/**
* roaring_bitmap_smallest returns the smallest value in the set.
* Returns UINT32_MAX if the set is empty.
*/
uint32_t roaring_bitmap_minimum(const roaring_bitmap_t *bm) {
    if (bm->high_low_container.size > 0) {
        void *container = bm->high_low_container.containers[0];
        uint8_t typecode = bm->high_low_container.typecodes[0];
        uint32_t key = bm->high_low_container.keys[0];
        uint32_t lowvalue = container_minimum(container, typecode);
        return lowvalue | (key << 16);
    }
    return UINT32_MAX;
}

/**
* roaring_bitmap_smallest returns the greatest value in the set.
* Returns 0 if the set is empty.
*/
uint32_t roaring_bitmap_maximum(const roaring_bitmap_t *bm) {
    if (bm->high_low_container.size > 0) {
        void *container =
            bm->high_low_container.containers[bm->high_low_container.size - 1];
        uint8_t typecode =
            bm->high_low_container.typecodes[bm->high_low_container.size - 1];
        uint32_t key =
            bm->high_low_container.keys[bm->high_low_container.size - 1];
        uint32_t lowvalue = container_maximum(container, typecode);
        return lowvalue | (key << 16);
    }
    return 0;
}

bool roaring_bitmap_select(const roaring_bitmap_t *bm, uint32_t rank,
                           uint32_t *element) {
    void *container;
    uint8_t typecode;
    uint16_t key;
    uint32_t start_rank = 0;
    int i = 0;
    bool valid = false;
    while (!valid && i < bm->high_low_container.size) {
        container = bm->high_low_container.containers[i];
        typecode = bm->high_low_container.typecodes[i];
        valid =
            container_select(container, typecode, &start_rank, rank, element);
        i++;
    }

    if (valid) {
        key = bm->high_low_container.keys[i - 1];
        *element |= (key << 16);
        return true;
    } else
        return false;
}

bool roaring_bitmap_intersect(const roaring_bitmap_t *x1,
                                     const roaring_bitmap_t *x2) {
    const int length1 = x1->high_low_container.size,
              length2 = x2->high_low_container.size;
    uint64_t answer = 0;
    int pos1 = 0, pos2 = 0;

    while (pos1 < length1 && pos2 < length2) {
        const uint16_t s1 = ra_get_key_at_index(& x1->high_low_container, pos1);
        const uint16_t s2 = ra_get_key_at_index(& x2->high_low_container, pos2);

        if (s1 == s2) {
            uint8_t container_type_1, container_type_2;
            void *c1 = ra_get_container_at_index(& x1->high_low_container, pos1,
                                                 &container_type_1);
            void *c2 = ra_get_container_at_index(& x2->high_low_container, pos2,
                                                 &container_type_2);
            if( container_intersect(c1, container_type_1, c2, container_type_2) ) return true;
            ++pos1;
            ++pos2;
        } else if (s1 < s2) {  // s1 < s2
            pos1 = ra_advance_until(& x1->high_low_container, s2, pos1);
        } else {  // s1 > s2
            pos2 = ra_advance_until(& x2->high_low_container, s1, pos2);
        }
    }
    return answer;
}


uint64_t roaring_bitmap_and_cardinality(const roaring_bitmap_t *x1,
                                        const roaring_bitmap_t *x2) {
    const int length1 = x1->high_low_container.size,
              length2 = x2->high_low_container.size;
    uint64_t answer = 0;
    int pos1 = 0, pos2 = 0;

    while (pos1 < length1 && pos2 < length2) {
        const uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, pos1);
        const uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, pos2);

        if (s1 == s2) {
            uint8_t container_type_1, container_type_2;
            void *c1 = ra_get_container_at_index(&x1->high_low_container, pos1,
                                                 &container_type_1);
            void *c2 = ra_get_container_at_index(&x2->high_low_container, pos2,
                                                 &container_type_2);
            answer += container_and_cardinality(c1, container_type_1, c2,
                                                container_type_2);
            ++pos1;
            ++pos2;
        } else if (s1 < s2) {  // s1 < s2
            pos1 = ra_advance_until(&x1->high_low_container, s2, pos1);
        } else {  // s1 > s2
            pos2 = ra_advance_until(&x2->high_low_container, s1, pos2);
        }
    }
    return answer;
}

double roaring_bitmap_jaccard_index(const roaring_bitmap_t *x1,
                                    const roaring_bitmap_t *x2) {
    const uint64_t c1 = roaring_bitmap_get_cardinality(x1);
    const uint64_t c2 = roaring_bitmap_get_cardinality(x2);
    const uint64_t inter = roaring_bitmap_and_cardinality(x1, x2);
    return (double)inter / (double)(c1 + c2 - inter);
}

uint64_t roaring_bitmap_or_cardinality(const roaring_bitmap_t *x1,
                                       const roaring_bitmap_t *x2) {
    const uint64_t c1 = roaring_bitmap_get_cardinality(x1);
    const uint64_t c2 = roaring_bitmap_get_cardinality(x2);
    const uint64_t inter = roaring_bitmap_and_cardinality(x1, x2);
    return c1 + c2 - inter;
}

uint64_t roaring_bitmap_andnot_cardinality(const roaring_bitmap_t *x1,
                                           const roaring_bitmap_t *x2) {
    const uint64_t c1 = roaring_bitmap_get_cardinality(x1);
    const uint64_t inter = roaring_bitmap_and_cardinality(x1, x2);
    return c1 - inter;
}

uint64_t roaring_bitmap_xor_cardinality(const roaring_bitmap_t *x1,
                                        const roaring_bitmap_t *x2) {
    const uint64_t c1 = roaring_bitmap_get_cardinality(x1);
    const uint64_t c2 = roaring_bitmap_get_cardinality(x2);
    const uint64_t inter = roaring_bitmap_and_cardinality(x1, x2);
    return c1 + c2 - 2 * inter;
}


/**
 * Check whether a range of values from range_start (included) to range_end (excluded) is present
 */
bool roaring_bitmap_contains_range(const roaring_bitmap_t *r, uint64_t range_start, uint64_t range_end) {
    if(range_end >= UINT64_C(0x100000000)) {
        range_end = UINT64_C(0x100000000);
    }
    if (range_start >= range_end) return true;  // empty range are always contained!
    if (range_end - range_start == 1) return roaring_bitmap_contains(r, (uint32_t)range_start);
    uint16_t hb_rs = (uint16_t)(range_start >> 16);
    uint16_t hb_re = (uint16_t)((range_end - 1) >> 16);
    const int32_t span = hb_re - hb_rs;
    const int32_t hlc_sz = ra_get_size(&r->high_low_container);
    if (hlc_sz < span + 1) {
      return false;
    }
    int32_t is = ra_get_index(&r->high_low_container, hb_rs);
    int32_t ie = ra_get_index(&r->high_low_container, hb_re);
    ie = (ie < 0 ? -ie - 1 : ie);
    if ((is < 0) || ((ie - is) != span)) {
       return false;
    }
    const uint32_t lb_rs = range_start & 0xFFFF;
    const uint32_t lb_re = ((range_end - 1) & 0xFFFF) + 1;
    uint8_t typecode;
    void *container = ra_get_container_at_index(&r->high_low_container, is, &typecode);
    if (hb_rs == hb_re) {
      return container_contains_range(container, lb_rs, lb_re, typecode);
    }
    if (!container_contains_range(container, lb_rs, 1 << 16, typecode)) {
      return false;
    }
    assert(ie < hlc_sz); // would indicate an algorithmic bug
    container = ra_get_container_at_index(&r->high_low_container, ie, &typecode);
    if (!container_contains_range(container, 0, lb_re, typecode)) {
        return false;
    }
    for (int32_t i = is + 1; i < ie; ++i) {
        container = ra_get_container_at_index(&r->high_low_container, i, &typecode);
        if (!container_is_full(container, typecode) ) {
          return false;
        }
    }
    return true;
}


bool roaring_bitmap_is_strict_subset(const roaring_bitmap_t *ra1,
                                            const roaring_bitmap_t *ra2) {
    return (roaring_bitmap_get_cardinality(ra2) >
                roaring_bitmap_get_cardinality(ra1) &&
            roaring_bitmap_is_subset(ra1, ra2));
}


/*
 * FROZEN SERIALIZATION FORMAT DESCRIPTION
 *
 * -- (beginning must be aligned by 32 bytes) --
 * <bitset_data> uint64_t[BITSET_CONTAINER_SIZE_IN_WORDS * num_bitset_containers]
 * <run_data>    rle16_t[total number of rle elements in all run containers]
 * <array_data>  uint16_t[total number of array elements in all array containers]
 * <keys>        uint16_t[num_containers]
 * <counts>      uint16_t[num_containers]
 * <typecodes>   uint8_t[num_containers]
 * <header>      uint32_t
 *
 * <header> is a 4-byte value which is a bit union of FROZEN_COOKIE (15 bits)
 * and the number of containers (17 bits).
 *
 * <counts> stores number of elements for every container.
 * Its meaning depends on container type.
 * For array and bitset containers, this value is the container cardinality minus one.
 * For run container, it is the number of rle_t elements (n_runs).
 *
 * <bitset_data>,<array_data>,<run_data> are flat arrays of elements of
 * all containers of respective type.
 *
 * <*_data> and <keys> are kept close together because they are not accessed
 * during deserilization. This may reduce IO in case of large mmaped bitmaps.
 * All members have their native alignments during deserilization except <header>,
 * which is not guaranteed to be aligned by 4 bytes.
 */

size_t roaring_bitmap_frozen_size_in_bytes(const roaring_bitmap_t *rb) {
    const roaring_array_t *ra = &rb->high_low_container;
    size_t num_bytes = 0;
    for (int32_t i = 0; i < ra->size; i++) {
        switch (ra->typecodes[i]) {
            case BITSET_CONTAINER_TYPE_CODE: {
                num_bytes += BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t);
                break;
            }
            case RUN_CONTAINER_TYPE_CODE: {
                const run_container_t *run =
                        (const run_container_t *) ra->containers[i];
                num_bytes += run->n_runs * sizeof(rle16_t);
                break;
            }
            case ARRAY_CONTAINER_TYPE_CODE: {
                const array_container_t *array =
                        (const array_container_t *) ra->containers[i];
                num_bytes += array->cardinality * sizeof(uint16_t);
                break;
            }
            default:
                __builtin_unreachable();
        }
    }
    num_bytes += (2 + 2 + 1) * ra->size; // keys, counts, typecodes
    num_bytes += 4; // header
    return num_bytes;
}

inline static void *arena_alloc(char **arena, size_t num_bytes) {
    char *res = *arena;
    *arena += num_bytes;
    return res;
}

void roaring_bitmap_frozen_serialize(const roaring_bitmap_t *rb, char *buf) {
    /*
     * Note: we do not require user to supply spicificly aligned buffer.
     * Thus we have to use memcpy() everywhere.
     */

    const roaring_array_t *ra = &rb->high_low_container;

    size_t bitset_zone_size = 0;
    size_t run_zone_size = 0;
    size_t array_zone_size = 0;
    for (int32_t i = 0; i < ra->size; i++) {
        switch (ra->typecodes[i]) {
            case BITSET_CONTAINER_TYPE_CODE: {
                bitset_zone_size +=
                        BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t);
                break;
            }
            case RUN_CONTAINER_TYPE_CODE: {
                const run_container_t *run =
                        (const run_container_t *) ra->containers[i];
                run_zone_size += run->n_runs * sizeof(rle16_t);
                break;
            }
            case ARRAY_CONTAINER_TYPE_CODE: {
                const array_container_t *array =
                        (const array_container_t *) ra->containers[i];
                array_zone_size += array->cardinality * sizeof(uint16_t);
                break;
            }
            default:
                __builtin_unreachable();
        }
    }

    uint64_t *bitset_zone = (uint64_t *)arena_alloc(&buf, bitset_zone_size);
    rle16_t *run_zone = (rle16_t *)arena_alloc(&buf, run_zone_size);
    uint16_t *array_zone = (uint16_t *)arena_alloc(&buf, array_zone_size);
    uint16_t *key_zone = (uint16_t *)arena_alloc(&buf, 2*ra->size);
    uint16_t *count_zone = (uint16_t *)arena_alloc(&buf, 2*ra->size);
    uint8_t *typecode_zone = (uint8_t *)arena_alloc(&buf, ra->size);
    uint32_t *header_zone = (uint32_t *)arena_alloc(&buf, 4);

    for (int32_t i = 0; i < ra->size; i++) {
        uint16_t count;
        switch (ra->typecodes[i]) {
            case BITSET_CONTAINER_TYPE_CODE: {
                const bitset_container_t *bitset =
                        (const bitset_container_t *) ra->containers[i];
                memcpy(bitset_zone, bitset->array,
                       BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t));
                bitset_zone += BITSET_CONTAINER_SIZE_IN_WORDS;
                if (bitset->cardinality != BITSET_UNKNOWN_CARDINALITY) {
                    count = bitset->cardinality - 1;
                } else {
                    count = bitset_container_compute_cardinality(bitset) - 1;
                }
                break;
            }
            case RUN_CONTAINER_TYPE_CODE: {
                const run_container_t *run =
                        (const run_container_t *) ra->containers[i];
                size_t num_bytes = run->n_runs * sizeof(rle16_t);
                memcpy(run_zone, run->runs, num_bytes);
                run_zone += run->n_runs;
                count = run->n_runs;
                break;
            }
            case ARRAY_CONTAINER_TYPE_CODE: {
                const array_container_t *array =
                        (const array_container_t *) ra->containers[i];
                size_t num_bytes = array->cardinality * sizeof(uint16_t);
                memcpy(array_zone, array->array, num_bytes);
                array_zone += array->cardinality;
                count = array->cardinality - 1;
                break;
            }
            default:
                __builtin_unreachable();
        }
        memcpy(&count_zone[i], &count, 2);
    }
    memcpy(key_zone, ra->keys, ra->size * sizeof(uint16_t));
    memcpy(typecode_zone, ra->typecodes, ra->size * sizeof(uint8_t));
    uint32_t header = ((uint32_t)ra->size << 15) | FROZEN_COOKIE;
    memcpy(header_zone, &header, 4);
}

const roaring_bitmap_t *
roaring_bitmap_frozen_view(const char *buf, size_t length) {
    if ((uintptr_t)buf % 32 != 0) {
        return NULL;
    }

    // cookie and num_containers
    if (length < 4) {
        return NULL;
    }
    uint32_t header;
    memcpy(&header, buf + length - 4, 4); // header may be misaligned
    if ((header & 0x7FFF) != FROZEN_COOKIE) {
        return NULL;
    }
    int32_t num_containers = (header >> 15);

    // typecodes, counts and keys
    if (length < 4 + (size_t)num_containers * (1 + 2 + 2)) {
        return NULL;
    }
    uint16_t *keys = (uint16_t *)(buf + length - 4 - num_containers * 5);
    uint16_t *counts = (uint16_t *)(buf + length - 4 - num_containers * 3);
    uint8_t *typecodes = (uint8_t *)(buf + length - 4 - num_containers * 1);

    // {bitset,array,run}_zone
    int32_t num_bitset_containers = 0;
    int32_t num_run_containers = 0;
    int32_t num_array_containers = 0;
    size_t bitset_zone_size = 0;
    size_t run_zone_size = 0;
    size_t array_zone_size = 0;
    for (int32_t i = 0; i < num_containers; i++) {
        switch (typecodes[i]) {
            case BITSET_CONTAINER_TYPE_CODE:
                num_bitset_containers++;
                bitset_zone_size += BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t);
                break;
            case RUN_CONTAINER_TYPE_CODE:
                num_run_containers++;
                run_zone_size += counts[i] * sizeof(rle16_t);
                break;
            case ARRAY_CONTAINER_TYPE_CODE:
                num_array_containers++;
                array_zone_size += (counts[i] + UINT32_C(1)) * sizeof(uint16_t);
                break;
            default:
                return NULL;
        }
    }
    if (length != bitset_zone_size + run_zone_size + array_zone_size +
                  5 * num_containers + 4) {
        return NULL;
    }
    uint64_t *bitset_zone = (uint64_t*) (buf);
    rle16_t *run_zone = (rle16_t*) (buf + bitset_zone_size);
    uint16_t *array_zone = (uint16_t*) (buf + bitset_zone_size + run_zone_size);

    size_t alloc_size = 0;
    alloc_size += sizeof(roaring_bitmap_t);
    alloc_size += num_containers * sizeof(void *);
    alloc_size += num_bitset_containers * sizeof(bitset_container_t);
    alloc_size += num_run_containers * sizeof(run_container_t);
    alloc_size += num_array_containers * sizeof(array_container_t);

    char *arena = (char *)malloc(alloc_size);
    if (arena == NULL) {
        return NULL;
    }

    roaring_bitmap_t *rb = (roaring_bitmap_t *)
            arena_alloc(&arena, sizeof(roaring_bitmap_t));
    rb->high_low_container.flags = ROARING_FLAG_FROZEN;
    rb->high_low_container.allocation_size = num_containers;
    rb->high_low_container.size = num_containers;
    rb->high_low_container.keys = (uint16_t *)keys;
    rb->high_low_container.typecodes = (uint8_t *)typecodes;
    rb->high_low_container.containers =
            (void **)arena_alloc(&arena, sizeof(void*) * num_containers);
    for (int32_t i = 0; i < num_containers; i++) {
        switch (typecodes[i]) {
            case BITSET_CONTAINER_TYPE_CODE: {
                bitset_container_t *bitset = (bitset_container_t *)
                        arena_alloc(&arena, sizeof(bitset_container_t));
                bitset->array = bitset_zone;
                bitset->cardinality = counts[i] + UINT32_C(1);
                rb->high_low_container.containers[i] = bitset;
                bitset_zone += BITSET_CONTAINER_SIZE_IN_WORDS;
                break;
            }
            case RUN_CONTAINER_TYPE_CODE: {
                run_container_t *run = (run_container_t *)
                        arena_alloc(&arena, sizeof(run_container_t));
                run->capacity = counts[i];
                run->n_runs = counts[i];
                run->runs = run_zone;
                rb->high_low_container.containers[i] = run;
                run_zone += run->n_runs;
                break;
            }
            case ARRAY_CONTAINER_TYPE_CODE: {
                array_container_t *array = (array_container_t *)
                        arena_alloc(&arena, sizeof(array_container_t));
                array->capacity = counts[i] + UINT32_C(1);
                array->cardinality = counts[i] + UINT32_C(1);
                array->array = array_zone;
                rb->high_low_container.containers[i] = array;
                array_zone += counts[i] + UINT32_C(1);
                break;
            }
            default:
                return NULL;
        }
    }

    return rb;
}
