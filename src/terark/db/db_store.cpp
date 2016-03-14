#include "db_store.hpp"

namespace terark { namespace db {

Permanentable::Permanentable() {
}
Permanentable::~Permanentable() {
}
void Permanentable::save(PathRef) const {
	THROW_STD(invalid_argument, "This method should not be called");
}
void Permanentable::load(PathRef) {
	THROW_STD(invalid_argument, "This method should not be called");
}

StoreIterator::~StoreIterator() {
}

///////////////////////////////////////////////////////////////////////////////
typedef hash_strmap< std::function<ReadableStore*()>
					, fstring_func::hash_align
					, fstring_func::equal_align
					, ValueInline, SafeCopy
					>
		StoreFactory;
static	StoreFactory s_storeFactory;

ReadableStore::RegisterStoreFactory::RegisterStoreFactory
(const char* fnameSuffix, const std::function<ReadableStore*()>& f)
{
	auto ib = s_storeFactory.insert_i(fnameSuffix, f);
	assert(ib.second);
	if (!ib.second)
		THROW_STD(invalid_argument, "duplicate suffix: %s", fnameSuffix);
}

ReadableStore::ReadableStore() {
	m_recordsBasePtr = nullptr;
}
ReadableStore::~ReadableStore() {
}

ReadableStore* ReadableStore::openStore(PathRef segDir, fstring fname) {
	size_t sufpos = fname.size();
	while (sufpos > 0 && fname[sufpos-1] != '.') --sufpos;
	auto suffix = fname.substr(sufpos);
	size_t idx = s_storeFactory.find_i(suffix);
	if (idx < s_storeFactory.end_i()) {
		const auto& factory = s_storeFactory.val(idx);
		ReadableStore* store = factory();
		assert(NULL != store);
		if (NULL == store) {
			THROW_STD(runtime_error, "store factory should not return NULL store");
		}
		auto fpath = segDir / fname.str();
		store->load(fpath);
		return store;
	}
	else {
		THROW_STD(invalid_argument
			, "store type '%.*s' of '%s' is not registered"
			, suffix.ilen(), suffix.data()
			, (segDir / fname.str()).string().c_str()
			);
		return NULL; // avoid compiler warning
	}
}

WritableStore* ReadableStore::getWritableStore() {
	return nullptr;
}

ReadableIndex* ReadableStore::getReadableIndex() {
	return nullptr;
}

AppendableStore* ReadableStore::getAppendableStore() {
	return nullptr;
}

UpdatableStore* ReadableStore::getUpdatableStore() {
	return nullptr;
}

namespace {
	class DefaultStoreIterForward : public StoreIterator {
		DbContextPtr m_ctx;
		llong m_rows;
		llong m_id;
	public:
		DefaultStoreIterForward(ReadableStore* store, DbContext* ctx) {
			m_store.reset(store);
			m_ctx.reset(ctx);
			m_rows = store->numDataRows();
			m_id = 0;
		}
		bool increment(llong* id, valvec<byte>* val) override {
			if (m_id < m_rows) {
				m_store->getValue(m_id, val, m_ctx.get());
				*id = m_id++;
				return true;
			}
			return false;
		}
		bool seekExact(llong  id, valvec<byte>* val) override {
			if (id <= m_rows) {
				m_id = m_rows;
				return true;
			}
			return false;
		}
		void reset() {
			m_rows = m_store->numDataRows();
			m_id = 0;
		}
	};
	class DefaultStoreIterBackward : public StoreIterator {
		DbContextPtr m_ctx;
		llong m_rows;
		llong m_id;
	public:
		DefaultStoreIterBackward(ReadableStore* store, DbContext* ctx) {
			m_store.reset(store);
			m_ctx.reset(ctx);
			m_rows = store->numDataRows();
			m_id = m_rows;
		}
		bool increment(llong* id, valvec<byte>* val) override {
			if (m_id > 0) {
				m_store->getValue(m_id, val, m_ctx.get());
				*id = --m_id;
				return true;
			}
			return false;
		}
		bool seekExact(llong  id, valvec<byte>* val) override {
			if (id <= m_rows) {
				m_id = m_rows;
				return true;
			}
			return false;
		}
		void reset() {
			m_rows = m_store->numDataRows();
			m_id = m_rows;
		}
	};
} // namespace

StoreIterator*
ReadableStore::createDefaultStoreIterForward(DbContext* ctx) const {
	return new DefaultStoreIterForward(const_cast<ReadableStore*>(this), ctx);
}
StoreIterator*
ReadableStore::createDefaultStoreIterBackward(DbContext* ctx) const {
	return new DefaultStoreIterBackward(const_cast<ReadableStore*>(this), ctx);
}

///////////////////////////////////////////////////////////////////////////////

AppendableStore::~AppendableStore() {
}

///////////////////////////////////////////////////////////////////////////////

UpdatableStore::~UpdatableStore() {
}

///////////////////////////////////////////////////////////////////////////////

WritableStore::~WritableStore() {
}

///////////////////////////////////////////////////////////////////////////////

MultiPartStore::MultiPartStore(valvec<ReadableStorePtr>& parts) {
	m_parts.swap(parts);
	syncRowNumVec();
}

MultiPartStore::~MultiPartStore() {
}

llong MultiPartStore::dataInflateSize() const {
	size_t size = 0;
	for (auto& part : m_parts)
		size += part->dataInflateSize();
	return size;
}
llong MultiPartStore::dataStorageSize() const {
	size_t size = 0;
	for (auto& part : m_parts)
		size += part->dataStorageSize();
	return size;
}

llong MultiPartStore::numDataRows() const {
	return m_rowNumVec.back();
}

void
MultiPartStore::getValueAppend(llong id, valvec<byte>* val, DbContext* ctx)
const {
	assert(m_parts.size() + 1 == m_rowNumVec.size());
	llong maxId = m_rowNumVec.back();
	if (id >= maxId) {
		THROW_STD(out_of_range, "id %lld, maxId = %lld", id, maxId);
	}
	size_t upp = upper_bound_a(m_rowNumVec, uint32_t(id));
	assert(upp < m_rowNumVec.size());
	llong baseId = m_rowNumVec[upp-1];
	m_parts[upp-1]->getValueAppend(id - baseId, val, ctx);
}

class MultiPartStore::MyStoreIterForward : public StoreIterator {
	size_t m_partIdx = 0;
	llong  m_id = 0;
	DbContextPtr m_ctx;
public:
	MyStoreIterForward(const MultiPartStore* owner, DbContext* ctx)
	  : m_ctx(ctx) {
		m_store.reset(const_cast<MultiPartStore*>(owner));
	}
	bool increment(llong* id, valvec<byte>* val) override {
		auto owner = static_cast<const MultiPartStore*>(m_store.get());
		assert(m_partIdx < owner->m_parts.size());
		if (terark_likely(m_id < owner->m_rowNumVec[m_partIdx + 1])) {
			// do nothing
		}
		else if (m_partIdx + 1 < owner->m_parts.size()) {
			m_partIdx++;
		}
		else {
			return false;
		}
		*id = m_id++;
		llong baseId = owner->m_rowNumVec[m_partIdx];
		llong subId = *id - baseId;
		owner->m_parts[m_partIdx]->getValue(subId, val, m_ctx.get());
		return true;
	}
	bool seekExact(llong id, valvec<byte>* val) override {
		auto owner = static_cast<const MultiPartStore*>(m_store.get());
		if (id < 0 || id >= owner->m_rowNumVec.back()) {
			return false;
		}
		size_t upp = upper_bound_a(owner->m_rowNumVec, id);
		llong  baseId = owner->m_rowNumVec[upp-1];
		llong  subId = id - baseId;
		owner->m_parts[upp-1]->getValue(subId, val, m_ctx.get());
		m_id = id+1;
		m_partIdx = upp-1;
		return true;
	}
	void reset() override {
		m_partIdx = 0;
		m_id = 0;
	}
};

class MultiPartStore::MyStoreIterBackward : public StoreIterator {
	size_t m_partIdx;
	llong  m_id;
	DbContextPtr m_ctx;
public:
	MyStoreIterBackward(const MultiPartStore* owner, DbContext* ctx)
	  : m_ctx(ctx) {
		m_store.reset(const_cast<MultiPartStore*>(owner));
		m_partIdx = owner->m_parts.size();
		m_id = owner->m_rowNumVec.back();
	}
	bool increment(llong* id, valvec<byte>* val) override {
		auto owner = static_cast<const MultiPartStore*>(m_store.get());
		if (owner->m_parts.empty()) {
			return false;
		}
		assert(m_partIdx > 0);
		if (terark_likely(m_id > owner->m_rowNumVec[m_partIdx-1])) {
			// do nothing
		}
		else if (m_partIdx > 1) {
			--m_partIdx;
		}
		else {
			return false;
		}
		*id = --m_id;
		llong baseId = owner->m_rowNumVec[m_partIdx-1];
		llong subId = *id - baseId;
		owner->m_parts[m_partIdx-1]->getValue(subId, val, m_ctx.get());
		return true;
	}
	bool seekExact(llong id, valvec<byte>* val) override {
		auto owner = static_cast<const MultiPartStore*>(m_store.get());
		if (id < 0 || id >= owner->m_rowNumVec.back()) {
			return false;
		}
		size_t upp = upper_bound_a(owner->m_rowNumVec, id);
		llong  baseId = owner->m_rowNumVec[upp-1];
		llong  subId = id - baseId;
		owner->m_parts[upp-1]->getValue(subId, val, m_ctx.get());
		m_partIdx = upp;
		m_id = id;
		return true;
	}
	void reset() override {
		auto owner = static_cast<const MultiPartStore*>(m_store.get());
		m_partIdx = owner->m_parts.size();
		m_id = owner->m_rowNumVec.back();
	}
};
StoreIterator* MultiPartStore::createStoreIterForward(DbContext* ctx) const {
	return new MyStoreIterForward(this, ctx);
}
StoreIterator* MultiPartStore::createStoreIterBackward(DbContext* ctx) const {
	return new MyStoreIterBackward(this, ctx);
}

void MultiPartStore::load(PathRef path) {
	abort();
}

void MultiPartStore::save(PathRef path) const {
	char szNum[16];
	for (size_t i = 0; i < m_parts.size(); ++i) {
		snprintf(szNum, sizeof(szNum), ".%04zd", i);
		m_parts[i]->save(path + szNum);
	}
}

void MultiPartStore::syncRowNumVec() {
	m_rowNumVec.resize_no_init(m_parts.size() + 1);
	llong rows = 0;
	for (size_t i = 0; i < m_parts.size(); ++i) {
		m_rowNumVec[i] = uint32_t(rows);
		rows += m_parts[i]->numDataRows();
	}
	m_rowNumVec.back() = uint32_t(rows);
}

/////////////////////////////////////////////////////////////////////////////

} } // namespace terark::db