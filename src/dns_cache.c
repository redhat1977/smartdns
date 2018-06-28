#include "dns_cache.h"
#include <pthread.h>

struct dns_cache_head {
	DECLARE_HASHTABLE(cache_hash, 10);
	struct list_head cache_list;
	int num;
	int size;
	pthread_mutex_t lock;
};

struct dns_cache_head dns_cache_head;

int dns_cache_init(int size)
{
	INIT_LIST_HEAD(&dns_cache_head.cache_list);
	hash_init(dns_cache_head.cache_hash);
	dns_cache_head.num = 0;
	dns_cache_head.size = size;

	pthread_mutex_init(&dns_cache_head.lock, 0);

	return 0;
}

struct dns_cache *_dns_cache_last(void)
{
	return list_last_entry(&dns_cache_head.cache_list, struct dns_cache, list);
}

struct dns_cache *_dns_cache_first(void)
{
	return list_first_entry(&dns_cache_head.cache_list, struct dns_cache, list);
}

void _dns_cache_delete(struct dns_cache *dns_cache)
{
	hash_del(&dns_cache->node);
	list_del_init(&dns_cache->list);
	dns_cache_head.num--;
	free(dns_cache);
}

void dns_cache_release(struct dns_cache *dns_cache)
{
	if (!atomic_dec_and_test(&dns_cache->ref)) {
		return;
	}

	_dns_cache_delete(dns_cache);
}

int dns_cache_insert(char *domain, int ttl, dns_type_t qtype, unsigned char *addr, int addr_len)
{
	unsigned int key = 0;
	struct dns_cache *dns_cache = NULL;

	if (dns_cache_head.size <= 0) {
		return 0;
	}

	dns_cache = dns_cache_get(domain, qtype);
	if (dns_cache) {
		dns_cache_release(dns_cache);
		return 0;
	}

	dns_cache = malloc(sizeof(*dns_cache));
	if (dns_cache == NULL) {
		goto errout;
	}

	key = hash_string(domain);
	key = jhash(&qtype, sizeof(qtype), key);
	strncpy(dns_cache->domain, domain, DNS_MAX_CNAME_LEN);
	dns_cache->qtype = qtype;
	dns_cache->ttl = ttl;
	atomic_set(&dns_cache->ref, 1);
	time(&dns_cache->insert_time);
	if (qtype == DNS_T_A) {
		if (addr_len != DNS_RR_A_LEN) {
			goto errout;
		}
		memcpy(dns_cache->addr, addr, DNS_RR_A_LEN);
	} else if (qtype == DNS_T_AAAA) {
		if (addr_len != DNS_RR_AAAA_LEN) {
			goto errout;
		}
		memcpy(dns_cache->addr, addr, DNS_RR_AAAA_LEN);
	} else {
		goto errout;
	}

	pthread_mutex_lock(&dns_cache_head.lock);
	hash_add(dns_cache_head.cache_hash, &dns_cache->node, key);
	list_add_tail(&dns_cache->list, &dns_cache_head.cache_list);

	dns_cache_head.num++;
	if (dns_cache_head.num > dns_cache_head.size) {
		struct dns_cache *del_cache;
		del_cache = _dns_cache_first();
		dns_cache_release(del_cache);
	}
	pthread_mutex_unlock(&dns_cache_head.lock);

	return 0;
errout:
	if (dns_cache) {
		free(dns_cache);
	}

	return -1;
}

struct dns_cache *dns_cache_get(char *domain, dns_type_t qtype)
{
	unsigned int key = 0;
	struct dns_cache *dns_cache = NULL;
	struct dns_cache *dns_cache_ret = NULL;
	time_t now;

	if (dns_cache_head.size <= 0) {
		return NULL;
	}

	key = hash_string(domain);
	key = jhash(&qtype, sizeof(qtype), key);

	time(&now);
	pthread_mutex_lock(&dns_cache_head.lock);
	hash_for_each_possible(dns_cache_head.cache_hash, dns_cache, node, key)
	{
		if (dns_cache->qtype != qtype) {
			continue;
		}

		if (strncmp(domain, dns_cache->domain, DNS_MAX_CNAME_LEN) != 0) {
			continue;
		}

		dns_cache_ret = dns_cache;
		break;
	}

	if (dns_cache_ret) {
		if (now - dns_cache_ret->insert_time > dns_cache_ret->ttl) {
			_dns_cache_delete(dns_cache_ret);
			dns_cache_ret = NULL;
		} else {
			atomic_inc(&dns_cache_ret->ref);
		}
	}

	pthread_mutex_unlock(&dns_cache_head.lock);

	return dns_cache_ret;
}

int dns_cache_get_ttl(struct dns_cache *dns_cache)
{
	time_t now;
	int ttl = 0;
	time(&now);

	ttl = dns_cache->insert_time + dns_cache->ttl - now;
	if (ttl < 0) {
		return 0;
	}

	return ttl;
}

void dns_cache_delete(struct dns_cache *dns_cache)
{
	pthread_mutex_lock(&dns_cache_head.lock);
	hash_del(&dns_cache->node);
	list_del_init(&dns_cache->list);
	pthread_mutex_unlock(&dns_cache_head.lock);
	dns_cache_release(dns_cache);
}

void dns_cache_update(struct dns_cache *dns_cache)
{
	pthread_mutex_lock(&dns_cache_head.lock);
	if (!list_empty(&dns_cache->list)) {
		list_del_init(&dns_cache->list);
		list_add_tail(&dns_cache->list, &dns_cache_head.cache_list);
	}
	pthread_mutex_unlock(&dns_cache_head.lock);
}

void dns_cache_invalidate(void)
{
	struct dns_cache *dns_cache = NULL;
	struct dns_cache *tmp;
	time_t now;
	int ttl = 0;

	if (dns_cache_head.size <= 0) {
		return;
	}

	time(&now);
	pthread_mutex_lock(&dns_cache_head.lock);
	list_for_each_entry_safe(dns_cache, tmp, &dns_cache_head.cache_list, list)
	{
		ttl = dns_cache->insert_time + dns_cache->ttl - now;
		if (ttl > 0) {
			break;
		}

		hash_del(&dns_cache->node);
		list_del_init(&dns_cache->list);
		dns_cache_release(dns_cache);
	}
	pthread_mutex_unlock(&dns_cache_head.lock);
}

void dns_cache_destroy(void)
{
	struct dns_cache *dns_cache = NULL;
	struct dns_cache *tmp;
	pthread_mutex_lock(&dns_cache_head.lock);
	list_for_each_entry_safe(dns_cache, tmp, &dns_cache_head.cache_list, list)
	{
		_dns_cache_delete(dns_cache);
	}
	pthread_mutex_unlock(&dns_cache_head.lock);

	pthread_mutex_destroy(&dns_cache_head.lock);
}