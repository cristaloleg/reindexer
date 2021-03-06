#include "transactionimpl.h"
namespace reindexer {

using std::string;

TransactionAccessor::TransactionAccessor(TransactionAccessor &&rhs) noexcept : Transaction(std::move(rhs)) {}
TransactionAccessor &TransactionAccessor::operator=(TransactionAccessor &&rhs) noexcept {
	Transaction::operator=(std::move(rhs));
	return *this;
}
const string &TransactionAccessor::GetName() { return impl_->nsName_; }

Completion TransactionAccessor::GetCmpl() { return impl_->cmpl_; }

vector<TransactionStep> &TransactionAccessor::GetSteps() { return impl_->steps_; }

void TransactionImpl::Insert(Item &&item) { steps_.emplace_back(TransactionStep{move(item), ModeInsert}); }
void TransactionImpl::Update(Item &&item) { steps_.emplace_back(TransactionStep{move(item), ModeUpdate}); }
void TransactionImpl::Upsert(Item &&item) { steps_.emplace_back(TransactionStep{move(item), ModeUpsert}); }
void TransactionImpl::Delete(Item &&item) { steps_.emplace_back(TransactionStep{move(item), ModeDelete}); }
void TransactionImpl::Modify(Item &&item, ItemModifyMode mode) { steps_.emplace_back(TransactionStep{move(item), mode}); }

}  // namespace reindexer
