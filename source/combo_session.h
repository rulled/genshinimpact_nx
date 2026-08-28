#ifndef GENSHIN_COMBO_SESSION_H
#define GENSHIN_COMBO_SESSION_H

#include <stddef.h>

typedef enum {
  COMBO_SESSION_LOAD_INVALID = -3,
  COMBO_SESSION_LOAD_IO_ERROR = -2,
  COMBO_SESSION_LOAD_KEY_UNAVAILABLE = -1,
  COMBO_SESSION_LOAD_MISSING = 0,
  COMBO_SESSION_LOAD_OK = 1,
} ComboSessionLoadResult;

/* Persist one genuine LoginManager success callback.  The callback and masked
 * label are encrypted and authenticated before they are written to the SD
 * card.  A console-bound key makes a copied session file unusable elsewhere;
 * this is deliberately not presented as an Android hardware keystore. */
int combo_session_store(const char *inner_json, size_t inner_size,
                        const char *asterisk_name);

/* Load, authenticate, decrypt, and structurally validate a saved callback.
 * The caller owns *inner_json_out and must erase it before free(). */
ComboSessionLoadResult combo_session_load(char **inner_json_out,
                                          size_t *inner_size_out,
                                          char *asterisk_name_out,
                                          size_t asterisk_name_capacity);

/* Delete only this wrapper's session and interrupted-write files. */
void combo_session_invalidate(void);

#endif
