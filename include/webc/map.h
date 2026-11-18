// SPDX-License-Identifier: Apache-2.0
// WebC — explicit map instantiation, open addressing with linear probing.
//
//   WEBC_MAP_DEFINE(MapStrAccount, map_str_account, str, Account)
//
// Key hashing and equality are chosen by the preprocessor, not by reflection:
// `str` compares by content, every other key type compares by bytes. A key type
// that needs something else declares WC_MAP_HASH_<Key>/WC_MAP_EQ_<Key> first.
#ifndef WEBC_MAP_H
#define WEBC_MAP_H

#include <webc/core.h>

typedef enum MapState : u8 { MAP_EMPTY = 0, MAP_USED = 1, MAP_TOMB = 2 } MapState;

// Both branches are cast to keep `_Generic` type-correct for every key type;
// only the selected branch is evaluated.
#define WC_HASH_KEY(K, k)                                                      \
  _Generic((K){ 0 },                                                           \
      str: wc_hash_str(*(const str *)&(k)),                                     \
      default: wc_hash_bytes(&(k), sizeof(K)))

#define WC_EQ_KEY(K, a, b)                                                     \
  _Generic((K){ 0 },                                                           \
      str: str_eq(*(const str *)&(a), *(const str *)&(b)),                      \
      default: memcmp(&(a), &(b), sizeof(K)) == 0)

#define WEBC_MAP_DEFINE(Name, prefix, K, V)                                    \
  typedef struct Name {                                                        \
    K *keys;                                                                   \
    V *vals;                                                                   \
    u8 *state;                                                                 \
    i32 cap, n, tombs;                                                         \
    Arena *arena;                                                              \
  } Name;                                                                      \
                                                                               \
  [[maybe_unused]] static inline Name prefix##_with(Arena *a_) {                                \
    return (Name){ .keys = nullptr, .vals = nullptr, .state = nullptr,          \
                   .cap = 0, .n = 0, .tombs = 0, .arena = a_ };                 \
  }                                                                            \
  [[maybe_unused]] static inline Name prefix##_new(void) { return prefix##_with(nullptr); }      \
                                                                               \
  [[maybe_unused]] static inline u64 prefix##_hash(K k_) { return WC_HASH_KEY(K, k_); }          \
  [[maybe_unused]] static inline bool prefix##_eq(K a_, K b_) { return WC_EQ_KEY(K, a_, b_); }   \
                                                                               \
  [[maybe_unused]] static inline void prefix##_rehash(Name *m_, i32 want_) {                     \
    i32 ocap_ = m_->cap;                                                        \
    K *okeys_ = m_->keys;                                                       \
    V *ovals_ = m_->vals;                                                       \
    u8 *ostate_ = m_->state;                                                    \
    i32 ncap_ = ocap_ ? ocap_ : 16;                                             \
    while (ncap_ < want_) ncap_ *= 2;                                           \
    m_->keys = m_->arena ? arena_alloc(m_->arena, (size_t)ncap_ * sizeof(K))     \
                         : wm_alloc((size_t)ncap_ * sizeof(K));                 \
    m_->vals = m_->arena ? arena_alloc(m_->arena, (size_t)ncap_ * sizeof(V))     \
                         : wm_alloc((size_t)ncap_ * sizeof(V));                 \
    m_->state = m_->arena ? arena_alloc(m_->arena, (size_t)ncap_)                \
                          : wm_alloc((size_t)ncap_);                            \
    for (i32 i_ = 0; i_ < ncap_; i_++) m_->state[i_] = MAP_EMPTY;                \
    m_->cap = ncap_;                                                            \
    m_->tombs = 0;                                                              \
    i32 old_n_ = m_->n;                                                         \
    m_->n = 0;                                                                  \
    for (i32 i_ = 0; i_ < ocap_; i_++) {                                        \
      if (ostate_[i_] != MAP_USED) continue;                                    \
      K k_ = okeys_[i_];                                                        \
      u64 h_ = prefix##_hash(k_) & (u64)(m_->cap - 1);                          \
      while (m_->state[h_] == MAP_USED) h_ = (h_ + 1) & (u64)(m_->cap - 1);     \
      m_->state[h_] = MAP_USED;                                                 \
      m_->keys[h_] = k_;                                                        \
      m_->vals[h_] = ovals_[i_];                                                \
      m_->n++;                                                                  \
    }                                                                           \
    wc_assert(m_->n == old_n_);                                                 \
    if (!m_->arena) {                                                           \
      if (okeys_) wm_free(okeys_);                                              \
      if (ovals_) wm_free(ovals_);                                              \
      if (ostate_) wm_free(ostate_);                                            \
    }                                                                           \
  }                                                                            \
                                                                               \
  [[maybe_unused]] static inline V *prefix##_slot(Name *m_, K k_, bool insert_) {                \
    if (m_->cap == 0) {                                                         \
      if (!insert_) return nullptr;                                             \
      prefix##_rehash(m_, 16);                                                  \
    } else if ((m_->n + m_->tombs + 1) * 10 >= m_->cap * 7) {                    \
      prefix##_rehash(m_, m_->cap * 2);                                         \
    }                                                                           \
    u64 mask_ = (u64)(m_->cap - 1);                                             \
    u64 h_ = prefix##_hash(k_) & mask_;                                         \
    i64 tomb_ = -1;                                                             \
    for (;;) {                                                                  \
      u8 st_ = m_->state[h_];                                                   \
      if (st_ == MAP_EMPTY) {                                                   \
        if (!insert_) return nullptr;                                           \
        i32 idx_ = tomb_ >= 0 ? (i32)tomb_ : (i32)h_;                           \
        if (tomb_ >= 0) m_->tombs--;                                            \
        m_->state[idx_] = MAP_USED;                                             \
        m_->keys[idx_] = k_;                                                    \
        m_->n++;                                                                \
        return &m_->vals[idx_];                                                 \
      }                                                                         \
      if (st_ == MAP_TOMB) {                                                    \
        if (tomb_ < 0) tomb_ = (i64)h_;                                         \
      } else if (prefix##_eq(m_->keys[h_], k_)) {                               \
        return &m_->vals[h_];                                                   \
      }                                                                         \
      h_ = (h_ + 1) & mask_;                                                    \
    }                                                                           \
  }                                                                            \
                                                                               \
  [[maybe_unused]] static inline V *prefix##_get(Name *m_, K k_) { return prefix##_slot(m_, k_, false); } \
                                                                               \
  [[maybe_unused]] static inline bool prefix##_has(Name *m_, K k_) { return prefix##_slot(m_, k_, false) != nullptr; } \
                                                                               \
  [[maybe_unused]] static inline V prefix##_get_or(Name *m_, K k_, V dflt_) {                    \
    V *v_ = prefix##_slot(m_, k_, false);                                       \
    return v_ ? *v_ : dflt_;                                                    \
  }                                                                            \
                                                                               \
  [[maybe_unused]] static inline void prefix##_put(Name *m_, K k_, V v_) {                       \
    V *slot_ = prefix##_slot(m_, k_, true);                                     \
    *slot_ = v_;                                                                \
  }                                                                            \
                                                                               \
  [[maybe_unused]] static inline bool prefix##_del(Name *m_, K k_) {                             \
    V *slot_ = prefix##_slot(m_, k_, false);                                    \
    if (!slot_) return false;                                                   \
    i32 idx_ = (i32)(slot_ - m_->vals);                                         \
    m_->state[idx_] = MAP_TOMB;                                                 \
    m_->n--;                                                                    \
    m_->tombs++;                                                                \
    return true;                                                                \
  }                                                                            \
                                                                               \
  [[maybe_unused]] static inline void prefix##_clear(Name *m_) {                                 \
    for (i32 i_ = 0; i_ < m_->cap; i_++) m_->state[i_] = MAP_EMPTY;             \
    m_->n = 0;                                                                  \
    m_->tombs = 0;                                                              \
  }                                                                            \
                                                                               \
  [[maybe_unused]] static inline void prefix##_free(Name *m_) {                                  \
    if (!m_->arena) {                                                           \
      if (m_->keys) wm_free(m_->keys);                                          \
      if (m_->vals) wm_free(m_->vals);                                          \
      if (m_->state) wm_free(m_->state);                                        \
    }                                                                           \
    m_->keys = nullptr;                                                         \
    m_->vals = nullptr;                                                         \
    m_->state = nullptr;                                                        \
    m_->cap = m_->n = m_->tombs = 0;                                            \
  }

// Prelude instantiations.
WEBC_MAP_DEFINE(MapStrStr, map_str_str, str, str)
WEBC_MAP_DEFINE(MapStrI64, map_str_i64, str, i64)
WEBC_MAP_DEFINE(MapStrPtr, map_str_ptr, str, void *)

// Iteration: the body runs only for live entries. `break` and `continue` behave.
#define map_iter(i_, m_)                                                       \
  for (i32 i_ = 0, _wc_n_ = (m_).cap; i_ < _wc_n_; i_++)                       \
    if ((m_).state[i_] != MAP_USED) continue; else

#endif // WEBC_MAP_H