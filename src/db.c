#include "redis.h"

#include <signal.h>

#include "win32fixes.h"
/*-----------------------------------------------------------------------------
 * C-level DB API
 *----------------------------------------------------------------------------*/

robj *lookupKey(redisDb *db, robj *key) {
    dictEntry *de = dictFind(db->dict,key->ptr);
    if (de) {
        robj *val = dictGetEntryVal(de);

        /* Update the access time for the aging algorithm.
         * Don't do it if we have a saving child, as this will trigger
         * a copy on write madness. */
        if (server.bgsavechildpid == -1 && server.bgrewritechildpid == -1)
            val->lru = server.lruclock;

        if (server.vm_enabled) {
            if (val->storage == REDIS_VM_MEMORY ||
                val->storage == REDIS_VM_SWAPPING)
            {
                /* If we were swapping the object out, cancel the operation */
                if (val->storage == REDIS_VM_SWAPPING)
                    vmCancelThreadedIOJob(val);
            } else {
                int notify = (val->storage == REDIS_VM_LOADING);

                /* Our value was swapped on disk. Bring it at home. */
                redisAssert(val->type == REDIS_VMPOINTER);
                val = vmLoadObject(val);
                dictGetEntryVal(de) = val;

                /* Clients blocked by the VM subsystem may be waiting for
                 * this key... */
                if (notify) handleClientsBlockedOnSwappedKey(db,key);
            }
        }
        server.stat_keyspace_hits++;
        return val;
    } else {
        server.stat_keyspace_misses++;
        return NULL;
    }
}

robj *lookupKeyRead(redisDb *db, robj *key) {
    expireIfNeeded(db,key);
    return lookupKey(db,key);
}

robj *lookupKeyWrite(redisDb *db, robj *key) {
    expireIfNeeded(db,key);
    return lookupKey(db,key);
}

robj *lookupKeyReadOrReply(redisClient *c, robj *key, robj *reply) {
    robj *o = lookupKeyRead(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

robj *lookupKeyWriteOrReply(redisClient *c, robj *key, robj *reply) {
    robj *o = lookupKeyWrite(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

/* Add the key to the DB. It's up to the caller to increment the reference
 * counte of the value if needed.
 *
 * The program is aborted if the key already exists. */
void dbAdd(redisDb *db, robj *key, robj *val) {
    sds copy = sdsdup(key->ptr);
    int retval = dictAdd(db->dict, copy, val);
    redisAssert(retval == REDIS_OK);
}

/* Overwrite an existing key with a new value. Incrementing the reference
 * count of the new value is up to the caller.
 * This function does not modify the expire time of the existing key.
 *
 * The program is aborted if the key was not already present. */
void dbOverwrite(redisDb *db, robj *key, robj *val) {
    struct dictEntry *de = dictFind(db->dict,key->ptr);
    
    redisAssert(de != NULL);
    dictReplace(db->dict, key->ptr, val);
}

/* High level Set operation. This function can be used in order to set
 * a key, whatever it was existing or not, to a new object.
 *
 * 1) The ref count of the value object is incremented.
 * 2) clients WATCHing for the destination key notified.
 * 3) The expire time of the key is reset (the key is made persistent). */
void setKey(redisDb *db, robj *key, robj *val) {
    if (lookupKeyWrite(db,key) == NULL) {
        dbAdd(db,key,val);
    } else {
        dbOverwrite(db,key,val);
    }
    incrRefCount(val);
    removeExpire(db,key);
    signalModifiedKey(db,key);
}

int dbExists(redisDb *db, robj *key) {
    return dictFind(db->dict,key->ptr) != NULL;
}

/* Return a random key, in form of a Redis object.
 * If there are no keys, NULL is returned.
 *
 * The function makes sure to return keys not already expired. */
robj *dbRandomKey(redisDb *db) {
    struct dictEntry *de;

    while(1) {
        sds key;
        robj *keyobj;

        de = dictGetRandomKey(db->dict);
        if (de == NULL) return NULL;

        key = dictGetEntryKey(de);
        keyobj = createStringObject(key,sdslen(key));
        if (dictFind(db->expires,key)) {
            if (expireIfNeeded(db,keyobj)) {
                decrRefCount(keyobj);
                continue; /* search for another key. This expired. */
            }
        }
        return keyobj;
    }
}

/* Delete a key, value, and associated expiration entry if any, from the DB */
int dbDelete(redisDb *db, robj *key) {
    /* If VM is enabled make sure to awake waiting clients for this key:
     * deleting the key will kill the I/O thread bringing the key from swap
     * to memory, so the client will never be notified and unblocked if we
     * don't do it now. */
    if (server.vm_enabled) handleClientsBlockedOnSwappedKey(db,key);
    /* Deleting an entry from the expires dict will not free the sds of
     * the key, because it is shared with the main dictionary. */
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);
    return dictDelete(db->dict,key->ptr) == DICT_OK;
}

/* Empty the whole database */
long long emptyDb() {
    int j;
    long long removed = 0;

    for (j = 0; j < server.dbnum; j++) {
        removed += dictSize(server.db[j].dict);
        dictEmpty(server.db[j].dict);
        dictEmpty(server.db[j].expires);
    }
    return removed;
}

int selectDb(redisClient *c, int id) {
    if (id < 0 || id >= server.dbnum)
        return REDIS_ERR;
    c->db = &server.db[id];
    return REDIS_OK;
}

/*-----------------------------------------------------------------------------
 * Hooks for key space changes.
 *
 * Every time a key in the database is modified the function
 * signalModifiedKey() is called.
 *
 * Every time a DB is flushed the function signalFlushDb() is called.
 *----------------------------------------------------------------------------*/

void signalModifiedKey(redisDb *db, robj *key) {
    touchWatchedKey(db,key);
}

void signalFlushedDb(int dbid) {
    touchWatchedKeysOnFlush(dbid);
}

/*-----------------------------------------------------------------------------
 * Type agnostic commands operating on the key space
 *----------------------------------------------------------------------------*/

void flushdbCommand(redisClient *c) {
    server.dirty += dictSize(c->db->dict);
    signalFlushedDb(c->db->id);
    dictEmpty(c->db->dict);
    dictEmpty(c->db->expires);
    addReply(c,shared.ok);
}

void flushallCommand(redisClient *c) {
    signalFlushedDb(-1);
    server.dirty += emptyDb();
    addReply(c,shared.ok);
    if (server.bgsavechildpid != -1) {
        kill(server.bgsavechildpid,SIGKILL);
        rdbRemoveTempFile(server.bgsavechildpid);
    }
    rdbSave(server.dbfilename);
    server.dirty++;
}

void delCommand(redisClient *c) {
    int deleted = 0, j;

    for (j = 1; j < c->argc; j++) {
        if (dbDelete(c->db,c->argv[j])) {
            signalModifiedKey(c->db,c->argv[j]);
            server.dirty++;
            deleted++;
        }
    }
    addReplyLongLong(c,deleted);
}

void existsCommand(redisClient *c) {
    expireIfNeeded(c->db,c->argv[1]);
    if (dbExists(c->db,c->argv[1])) {
        addReply(c, shared.cone);
    } else {
        addReply(c, shared.czero);
    }
}

void selectCommand(redisClient *c) {
    int id = atoi(c->argv[1]->ptr);

    if (selectDb(c,id) == REDIS_ERR) {
        addReplyError(c,"invalid DB index");
    } else {
        addReply(c,shared.ok);
    }
}

void randomkeyCommand(redisClient *c) {
    robj *key;

    if ((key = dbRandomKey(c->db)) == NULL) {
        addReply(c,shared.nullbulk);
        return;
    }

    addReplyBulk(c,key);
    decrRefCount(key);
}

void keysCommand(redisClient *c) {
    dictIterator *di;
    dictEntry *de;
    sds pattern = c->argv[1]->ptr;
    int plen = sdslen(pattern), allkeys;
#ifdef _WIN32
    size_t numkeys = 0;
#else
    unsigned long numkeys = 0;
#endif
    void *replylen = addDeferredMultiBulkLength(c);

    di = dictGetIterator(c->db->dict);
    allkeys = (pattern[0] == '*' && pattern[1] == '\0');
    while((de = dictNext(di)) != NULL) {
        sds key = dictGetEntryKey(de);
        robj *keyobj;

        if (allkeys || stringmatchlen(pattern,plen,key,sdslen(key),0)) {
            keyobj = createStringObject(key,sdslen(key));
            if (expireIfNeeded(c->db,keyobj) == 0) {
                addReplyBulk(c,keyobj);
                numkeys++;
            }
            decrRefCount(keyobj);
        }
    }
    dictReleaseIterator(di);
    setDeferredMultiBulkLength(c,replylen,numkeys);
}

void dbsizeCommand(redisClient *c) {
    addReplyLongLong(c,dictSize(c->db->dict));
}

void lastsaveCommand(redisClient *c) {
    addReplyLongLong(c,server.lastsave);
}

void typeCommand(redisClient *c) {
    robj *o;
    char *type;

    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        type = "none";
    } else {
        switch(o->type) {
        case REDIS_STRING: type = "string"; break;
        case REDIS_LIST: type = "list"; break;
        case REDIS_SET: type = "set"; break;
        case REDIS_ZSET: type = "zset"; break;
        case REDIS_HASH: type = "hash"; break;
        default: type = "unknown"; break;
        }
    }
    addReplyStatus(c,type);
}

void saveCommand(redisClient *c) {
    if (server.bgsavechildpid != -1) {
        addReplyError(c,"Background save already in progress");
        return;
    }
    if (rdbSave(server.dbfilename) == REDIS_OK) {
        addReply(c,shared.ok);
    } else {
        addReply(c,shared.err);
    }
}

void bgsaveCommand(redisClient *c) {
    if (server.bgsavechildpid != -1) {
        addReplyError(c,"Background save already in progress");
    } else if (server.bgrewritechildpid != -1) {
        addReplyError(c,"Can't BGSAVE while AOF log rewriting is in progress");
    } else if (rdbSaveBackground(server.dbfilename) == REDIS_OK) {
        addReplyStatus(c,"Background saving started");
    } else {
        addReply(c,shared.err);
    }
}

void shutdownCommand(redisClient *c) {
    if (prepareForShutdown() == REDIS_OK)
        exit(0);
    addReplyError(c,"Errors trying to SHUTDOWN. Check logs.");
}

void renameGenericCommand(redisClient *c, int nx) {
    robj *o;

    /* To use the same key as src and dst is probably an error */
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) {
        addReply(c,shared.sameobjecterr);
        return;
    }

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;

    incrRefCount(o);
    if (lookupKeyWrite(c->db,c->argv[2]) != NULL) {
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        dbOverwrite(c->db,c->argv[2],o);
    } else {
        dbAdd(c->db,c->argv[2],o);
    }
    dbDelete(c->db,c->argv[1]);
    signalModifiedKey(c->db,c->argv[1]);
    signalModifiedKey(c->db,c->argv[2]);
    server.dirty++;
    addReply(c,nx ? shared.cone : shared.ok);
}

void renameCommand(redisClient *c) {
    renameGenericCommand(c,0);
}

void renamenxCommand(redisClient *c) {
    renameGenericCommand(c,1);
}

void moveCommand(redisClient *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid;

    /* Obtain source and target DB pointers */
    src = c->db;
    srcid = c->db->id;
    if (selectDb(c,atoi(c->argv[2]->ptr)) == REDIS_ERR) {
        addReply(c,shared.outofrangeerr);
        return;
    }
    dst = c->db;
    selectDb(c,srcid); /* Back to the source DB */

    /* If the user is moving using as target the same
     * DB as the source DB it is probably an error. */
    if (src == dst) {
        addReply(c,shared.sameobjecterr);
        return;
    }

    /* Check if the element exists and get a reference */
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (!o) {
        addReply(c,shared.czero);
        return;
    }

    /* Return zero if the key already exists in the target DB */
    if (lookupKeyWrite(dst,c->argv[1]) != NULL) {
        addReply(c,shared.czero);
        return;
    }
    dbAdd(dst,c->argv[1],o);
    incrRefCount(o);

    /* OK! key moved, free the entry in the source DB */
    dbDelete(src,c->argv[1]);
    server.dirty++;
    addReply(c,shared.cone);
}

/*-----------------------------------------------------------------------------
 * Expires API
 *----------------------------------------------------------------------------*/

int removeExpire(redisDb *db, robj *key) {
    /* An expire may only be removed if there is a corresponding entry in the
     * main dict. Otherwise, the key will never be freed. */
    redisAssert(dictFind(db->dict,key->ptr) != NULL);
    return dictDelete(db->expires,key->ptr) == DICT_OK;
}

void setExpire(redisDb *db, robj *key, time_t when) {
    dictEntry *de;

    /* Reuse the sds from the main dict in the expire dict */
    de = dictFind(db->dict,key->ptr);
    redisAssert(de != NULL);
    dictReplace(db->expires,dictGetEntryKey(de),(void*)when);
}

/* Return the expire time of the specified key, or -1 if no expire
 * is associated with this key (i.e. the key is non volatile) */
time_t getExpire(redisDb *db, robj *key) {
    dictEntry *de;

    /* No expire? return ASAP */
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key->ptr)) == NULL) return -1;

    /* The entry was found in the expire dict, this means it should also
     * be present in the main dict (safety check). */
    redisAssert(dictFind(db->dict,key->ptr) != NULL);
    return (time_t) dictGetEntryVal(de);
}

/* Propagate expires into slaves and the AOF file.
 * When a key expires in the master, a DEL operation for this key is sent
 * to all the slaves and the AOF file if enabled.
 *
 * This way the key expiry is centralized in one place, and since both
 * AOF and the master->slave link guarantee operation ordering, everything
 * will be consistent even if we allow write operations against expiring
 * keys. */
void propagateExpire(redisDb *db, robj *key) {
    robj *argv[2];

    argv[0] = createStringObject("DEL",3);
    argv[1] = key;
    incrRefCount(key);

    if (server.appendonly)
        feedAppendOnlyFile(server.delCommand,db->id,argv,2);
    if (listLength(server.slaves))
        replicationFeedSlaves(server.slaves,db->id,argv,2);

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

int expireIfNeeded(redisDb *db, robj *key) {
    time_t when = getExpire(db,key);

    if (when < 0) return 0; /* No expire for this key */

    /* Don't expire anything while loading. It will be done later. */
    if (server.loading) return 0;

    /* If we are running in the context of a slave, return ASAP:
     * the slave key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys.
     *
     * Still we try to return the right information to the caller,
     * that is, 0 if we think the key should be still valid, 1 if
     * we think the key is expired at this time. */
    if (server.masterhost != NULL) {
        return time(NULL) > when;
    }

    /* Return when this key has not expired */
    if (time(NULL) <= when) return 0;

    /* Delete the key */
    server.stat_expiredkeys++;
    propagateExpire(db,key);
    return dbDelete(db,key);
}

/*-----------------------------------------------------------------------------
 * Expires Commands
 *----------------------------------------------------------------------------*/

void expireGenericCommand(redisClient *c, robj *key, robj *param, ssize_t offset) {
    dictEntry *de;
#ifdef _WIN64
    long long seconds;
#else
    long seconds;
#endif    

    if (getLongFromObjectOrReply(c, param, &seconds, NULL) != REDIS_OK) return;

    seconds -= offset;

    de = dictFind(c->db->dict,key->ptr);
    if (de == NULL) {
        addReply(c,shared.czero);
        return;
    }
    /* EXPIRE with negative TTL, or EXPIREAT with a timestamp into the past
     * should never be executed as a DEL when load the AOF or in the context
     * of a slave instance.
     *
     * Instead we take the other branch of the IF statement setting an expire
     * (possibly in the past) and wait for an explicit DEL from the master. */
    if (seconds <= 0 && !server.loading && !server.masterhost) {
        robj *aux;

        redisAssert(dbDelete(c->db,key));
        server.dirty++;

        /* Replicate/AOF this as an explicit DEL. */
        aux = createStringObject("DEL",3);
        rewriteClientCommandVector(c,2,aux,key);
        decrRefCount(aux);
        signalModifiedKey(c->db,key);
        addReply(c, shared.cone);
        return;
    } else {
        time_t when = time(NULL)+seconds;
        setExpire(c->db,key,when);
        addReply(c,shared.cone);
        signalModifiedKey(c->db,key);
        server.dirty++;
        return;
    }
}

void expireCommand(redisClient *c) {
    expireGenericCommand(c,c->argv[1],c->argv[2],0);
}

void expireatCommand(redisClient *c) {
    expireGenericCommand(c,c->argv[1],c->argv[2],time(NULL));
}

void ttlCommand(redisClient *c) {
    time_t expire, ttl = -1;

    expire = getExpire(c->db,c->argv[1]);
    if (expire != -1) {
        ttl = (expire-time(NULL));
        if (ttl < 0) ttl = -1;
    }
    addReplyLongLong(c,(long long)ttl);
}

void persistCommand(redisClient *c) {
    dictEntry *de;

    de = dictFind(c->db->dict,c->argv[1]->ptr);
    if (de == NULL) {
        addReply(c,shared.czero);
    } else {
        if (removeExpire(c->db,c->argv[1])) {
            addReply(c,shared.cone);
            server.dirty++;
        } else {
            addReply(c,shared.czero);
        }
    }
}
