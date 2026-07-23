#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/verify_order_book_hot_path.sh --schema SCHEMA BINARY [DISASSEMBLY_REPORT]

Disassemble the Release ITCH replay benchmark and fail when the book-mutation
closure contains an allocator, mapping, syscall, lock, or indirect call.
Supported schemas are `redesign_v1` and `branch5_native_v1`; unknown or absent
schemas fail closed. The optional report contains every selected function body
used by the schema-specific audit.
USAGE
}

die() {
  echo "hot-path verifier: $*" >&2
  exit 2
}

if [[ $# -lt 3 || $# -gt 4 || "$1" != --schema ]]; then
  usage >&2
  exit 2
fi

SCHEMA="$2"
BINARY="$3"
REPORT="${4:-}"
case "${SCHEMA}" in
  redesign_v1|branch5_native_v1) ;;
  *) die "unsupported hot-path schema: ${SCHEMA:-missing}" ;;
esac
[[ -f "${BINARY}" && -x "${BINARY}" ]] ||
  die "binary is not an executable file: ${BINARY}"
command -v objdump >/dev/null 2>&1 || die "objdump is required"
command -v awk >/dev/null 2>&1 || die "awk is required"

temporary_disassembly="$(mktemp "${TMPDIR:-/tmp}/astra-hot-all.XXXXXX")"
temporary_selected="$(mktemp "${TMPDIR:-/tmp}/astra-hot-selected.XXXXXX")"
cleanup() {
  rm -f -- "${temporary_disassembly}" "${temporary_selected}"
}
trap cleanup EXIT HUP INT TERM

objdump -d -C "${BINARY}" >"${temporary_disassembly}"

# Constructors/destructors and administrative preparation intentionally map,
# prefault, or destroy storage. They are excluded. The schema contract supplies
# the known parser/mutation roots, then every direct in-binary call or tail-call
# target is added recursively. This keeps a newly outlined project helper in
# the audit even when its symbol does not match one of the class-name patterns.
awk -v schema="${SCHEMA}" '
  function is_header(line) {
    return line ~ /^[[:space:]]*[[:xdigit:]]+[[:space:]]+<.*>:[[:space:]]*$/
  }
  function header_address(line, address) {
    address = line
    sub(/^[[:space:]]*/, "", address)
    sub(/[[:space:]].*$/, "", address)
    return canonical_address(address)
  }
  function header_symbol(line, symbol) {
    symbol = line
    sub(/^[[:space:]]*[[:xdigit:]]+[[:space:]]+</, "", symbol)
    sub(/>:[[:space:]]*$/, "", symbol)
    return symbol
  }
  function canonical_address(address) {
    address = tolower(address)
    sub(/^0x/, "", address)
    sub(/^0+/, "", address)
    return address == "" ? "0" : address
  }
  function direct_target_address(line, prefix, address) {
    if (line !~ /[[:space:]](callq?|jmpq?|bl|b)[[:space:]]+(0x)?[[:xdigit:]]+[[:space:]]+</)
      return ""
    prefix = line
    sub(/[[:space:]]+<.*$/, "", prefix)
    sub(/^.*[[:space:]]/, "", prefix)
    if (prefix !~ /^(0x)?[[:xdigit:]]+$/)
      return ""
    return canonical_address(prefix)
  }
  function is_schema_root(symbol, selected) {
    if (schema == "redesign_v1") {
      selected = symbol ~ /^(BookManager|OrderBook)::/ ||
                 symbol ~ /^astra::book::(OrderTable|PriceLevelStore)::/
      if (!selected)
        return 0
      if (symbol ~ /::~/ ||
          symbol ~ /::(BookManager|OrderBook|OrderTable|PriceLevelStore)\(/ ||
          symbol ~ /OrderBook::OwnedStores::/ ||
          symbol ~ /BookManager::getOrCreate\(/ ||
          symbol ~ /OrderTable::validateConfig\(/ ||
          symbol ~ /PriceLevelStore::prepareBook\(/)
        return 0
      return 1
    }
    if (schema == "branch5_native_v1") {
      if (symbol ~ /^(BookManager|OrderBook)::(addOrder|trade|cancelShares|deleteOrder|replaceOrder|addOrderIndexed|tradeIndexed|cancelSharesIndexed|deleteOrderIndexed|replaceOrderIndexed|removeFromLevel)\(/)
        return 1
      if (symbol ~ /^\(anonymous namespace\)::(appendOrder|unlinkOrder)\(/)
        return 1
      if (symbol ~ /^OrderArena::/ &&
          symbol !~ /^OrderArena::(OrderArena|checkedCapacity|stats)\(/)
        return 1
      if (symbol ~ /^LocalOrderRefMap::/ &&
          symbol !~ /^LocalOrderRefMap::LocalOrderRefMap\(/)
        return 1
      if (symbol ~ /^PriceLevelIndex::/ &&
          symbol !~ /^PriceLevelIndex::(PriceLevelIndex|~PriceLevelIndex|clear)\(/)
        return 1
      if (symbol ~ /^PriceLevelArena::/ &&
          symbol !~ /^PriceLevelArena::(PriceLevelArena|stats|availableLeaves|availableLevels|availableInternalNodes)\(/)
        return 1
    }
    return 0
  }
  function is_parser_root(symbol) {
    if (schema == "redesign_v1")
      return symbol ~ /^ItchParser::(handleMessage|handlePrevalidatedMessage|dispatchMessage|handleAdd|handleOrderExecuted|handleOrderExecutedWithPrice|applyExecution|handleCancel|handleDelete|handleReplace|applyBookResult|fail|applyBookFailure)\(/
    return symbol ~ /^ItchParser::(handleMessage|handleAdd|handleExecution|handleCancel|handleDelete|handleReplace)\(/
  }
  # Redesign error formatting must stay in these named noinline functions.
  # Their bodies are reported and audited, but their allocator calls and
  # outgoing library closure belong to a rejected-message/mutation path.
  function is_cold_error_boundary(symbol) {
    return symbol ~ /^ItchParser::(fail|applyBookFailure)\(/
  }
  # These targets handle non-book/admin cases and do not belong to a successful
  # A/F/E/C/X/D/U mutation closure.
  function is_admin_parser_boundary(symbol) {
    return symbol ~ /^ItchParser::(skip|handleBrokenTrade|handleStockDirectory|handleSystemEvent|handleSystemHoursStart|advancePhase|markStartupAdminMessage|createRegisteredBook|createRegisteredBooks)\(/
  }
  # Do not recurse into linked standard-library/runtime/PLT bodies. Calls to
  # forbidden targets are checked at the selected caller, before this boundary
  # matters. All other resolved in-binary symbols are presumed project-owned.
  function is_external_boundary(symbol) {
    return symbol ~ /@plt/ ||
           symbol ~ /(^|[.])plt([.]|$)/ ||
           symbol ~ /^(std::|__gnu_cxx::|__cxxabiv1::)/ ||
           symbol ~ /^(typeinfo for |vtable for |VTT for |construction vtable for |non-virtual thunk to |virtual thunk to )/ ||
           symbol ~ /^__/ ||
           symbol ~ /^operator (new|delete)/ ||
           symbol ~ /^_?(memcpy|memmove|memset|memcmp|strlen|strcmp|strncmp|abort|terminate)(@.*)?$/
  }
  {
    lines[NR] = $0
    if (is_header($0)) {
      if (function_count > 0)
        last_line[function_count] = NR - 1
      ++function_count
      first_line[function_count] = NR
      symbols[function_count] = header_symbol($0)
      addresses[function_count] = header_address($0)
      function_at[addresses[function_count]] = function_count
      current_function = function_count
    }
    owners[NR] = current_function
  }
  END {
    if (function_count > 0)
      last_line[function_count] = NR

    for (function_number = 1; function_number <= function_count;
         ++function_number) {
      if (is_schema_root(symbols[function_number]) ||
          is_parser_root(symbols[function_number]))
        selected[function_number] = 1
    }

    do {
      changed = 0
      for (function_number = 1; function_number <= function_count;
           ++function_number) {
        if (!selected[function_number] || expanded[function_number])
          continue
        expanded[function_number] = 1
        if (is_cold_error_boundary(symbols[function_number]))
          continue
        for (line_number = first_line[function_number] + 1;
             line_number <= last_line[function_number]; ++line_number) {
          target_address = direct_target_address(lines[line_number])
          if (target_address == "" || !(target_address in function_at))
            continue
          target_function = function_at[target_address]
          target_symbol = symbols[target_function]
          if (selected[target_function] ||
              is_admin_parser_boundary(target_symbol) ||
              is_external_boundary(target_symbol))
            continue
          selected[target_function] = 1
          changed = 1
        }
      }
    } while (changed)

    for (line_number = 1; line_number <= NR; ++line_number) {
      if (selected[owners[line_number]])
        print lines[line_number]
    }
  }
' "${temporary_disassembly}" >"${temporary_selected}"

selected_functions="$(
  awk '/^[[:space:]]*[[:xdigit:]]+[[:space:]]+<.*>:[[:space:]]*$/ { ++n }
       END { print n + 0 }' "${temporary_selected}"
)"

if [[ "${SCHEMA}" == redesign_v1 ]]; then
  [[ "${selected_functions}" -ge 40 ]] ||
    die "selected only ${selected_functions} redesign functions; symbol contract changed"
  REQUIRED_SYMBOLS=(
    'BookManager::addOrder('
    'BookManager::executeOrder('
    'BookManager::cancelShares('
    'BookManager::deleteOrder('
    'BookManager::replaceOrder('
    'OrderBook::addOrder('
    'OrderBook::executeOrder('
    'OrderBook::cancelShares('
    'OrderBook::deleteOrder('
    'OrderBook::replaceOrder('
    'astra::book::OrderTable::reserve('
    'astra::book::OrderTable::commit('
    'astra::book::OrderTable::findFallbackState('
    'astra::book::PriceLevelStore::add('
    'astra::book::PriceLevelStore::reduceChecked('
    'astra::book::PriceLevelStore::erase('
    'astra::book::PriceLevelStore::move('
    'astra::book::PriceLevelStore::nextWorse('
    'astra::book::PriceLevelStore::topTen('
    'ItchParser::handleMessage('
    'ItchParser::handlePrevalidatedMessage('
    'ItchParser::dispatchMessage('
    'ItchParser::handleAdd('
    'ItchParser::handleOrderExecuted('
    'ItchParser::handleOrderExecutedWithPrice('
    'ItchParser::applyExecution('
    'ItchParser::handleCancel('
    'ItchParser::handleDelete('
    'ItchParser::handleReplace('
    'ItchParser::applyBookResult('
    'ItchParser::fail('
    'ItchParser::applyBookFailure('
  )
else
  [[ "${selected_functions}" -ge 30 ]] ||
    die "selected only ${selected_functions} branch5 functions; symbol contract changed"
  REQUIRED_SYMBOLS=(
    'BookManager::addOrder('
    'BookManager::trade('
    'BookManager::cancelShares('
    'BookManager::deleteOrder('
    'BookManager::replaceOrder('
    'OrderBook::addOrder('
    'OrderBook::trade('
    'OrderBook::cancelShares('
    'OrderBook::deleteOrder('
    'OrderBook::replaceOrder('
    'OrderBook::addOrderIndexed('
    'OrderBook::tradeIndexed('
    'OrderBook::cancelSharesIndexed('
    'OrderBook::deleteOrderIndexed('
    'OrderBook::replaceOrderIndexed('
    'OrderBook::removeFromLevel('
    'OrderArena::allocate('
    'OrderArena::release('
    'OrderArena::at('
    'OrderArena::isInUse('
    'OrderArena::markFree('
    'OrderArena::markInUse('
    'LocalOrderRefMap::erase('
    'LocalOrderRefMap::replaceKey('
    'PriceLevelIndex::ensure('
    'PriceLevelIndex::find('
    'PriceLevelIndex::eraseEmpty('
    'PriceLevelIndex::best('
    'PriceLevelIndex::nextWorse('
    'PriceLevelArena::allocateNode('
    'PriceLevelArena::allocateLeaf('
    'PriceLevelArena::allocateLevel('
    'PriceLevelArena::releaseNode('
    'PriceLevelArena::releaseLeaf('
    'PriceLevelArena::releaseLevel('
    'PriceLevelArena::level('
    'PriceLevelArena::node('
    'PriceLevelArena::leaf('
    'ItchParser::handleMessage('
    'ItchParser::handleAdd('
    'ItchParser::handleExecution('
    'ItchParser::handleCancel('
    'ItchParser::handleDelete('
    'ItchParser::handleReplace('
  )
fi

for required_symbol in "${REQUIRED_SYMBOLS[@]}"; do
  awk -v required="${required_symbol}" '
    /^[[:space:]]*[[:xdigit:]]+[[:space:]]+<.*>:[[:space:]]*$/ &&
        index($0, "<" required) { found = 1 }
    END { exit found ? 0 : 1 }
  ' "${temporary_selected}" ||
    die "required hot-path symbol is absent: ${required_symbol}"
done

# Redesign validation and mutation-result strings are isolated in two required
# noinline cold helpers. Only those helpers may allocate; all redesign parser
# entry/dispatch/mutation roots use the normal zero-allocation rule. The pinned
# branch-5 parser predates that split, so its explicit historical parser roots
# retain their cold-error allocator exception. Mapping, syscalls, and locks are
# forbidden under both schemas.
forbidden_matches="$(awk -v schema="${SCHEMA}" '
  function is_header(line) {
    return line ~ /^[[:space:]]*[[:xdigit:]]+[[:space:]]+<.*>:[[:space:]]*$/
  }
  function header_symbol(line, symbol) {
    symbol = line
    sub(/^[[:space:]]*[[:xdigit:]]+[[:space:]]+</, "", symbol)
    sub(/>:[[:space:]]*$/, "", symbol)
    return symbol
  }
  function is_call_or_tail(line) {
    return line ~ /[[:space:]](callq?|bl|b|jmpq?)[[:space:]]/
  }
  function allocator_exception(symbol) {
    if (schema == "redesign_v1")
      return symbol ~ /^ItchParser::(fail|applyBookFailure)\(/
    return symbol ~ /^ItchParser::(handleMessage|handleAdd|handleExecution|handleCancel|handleDelete|handleReplace|fail)\(/
  }
  function has_kernel_entry_instruction(instruction) {
    return instruction ~ /[[:space:]](syscall|sysenter|svc|ecall|swi)([[:space:]]|$)/ ||
           instruction ~ /[[:space:]]int(q|l)?[[:space:]]+[$]?0x80([^[:xdigit:]]|$)/
  }
  {
    if (is_header($0))
      caller = header_symbol($0)
    lower = tolower($0)
    instruction = lower
    sub(/[[:space:]]+<.*$/, "", instruction)
    if (has_kernel_entry_instruction(instruction)) {
      print
      next
    }
    if (!is_call_or_tail(lower))
      next
    allocator = lower ~ /operator new|operator delete|(^|[^[:alnum:]_])(malloc|calloc|realloc|free)([^[:alnum:]_]|$)/
    hard_forbidden = lower ~ /(^|[^[:alnum:]_])(mmap|munmap|madvise|syscall|futex|pthread_[[:alnum:]_]*|mutex_[[:alnum:]_]*)([^[:alnum:]_]|$)/
    if (hard_forbidden || (allocator && !allocator_exception(caller)))
      print
  }
' "${temporary_selected}")"
if [[ -n "${forbidden_matches}" ]]; then
  printf '%s\n' "${forbidden_matches}"
  die "forbidden allocator/mapping/syscall/lock target in hot closure"
fi

# Reject virtual/function-pointer calls and indirect tail calls. Direct branch
# targets always carry an immediate address or symbol in supported objdump
# output; x86 uses * operands, while AArch64 uses br/blr registers. Redesign
# dispatch has one compiler-generated switch jump table. The pinned branch-5
# parser has two (its prepared-universe type gate plus message dispatch).
# Register calls are never allowed.
indirect_matches="$(awk -v schema="${SCHEMA}" '
  function is_header(line) {
    return line ~ /^[[:space:]]*[[:xdigit:]]+[[:space:]]+<.*>:[[:space:]]*$/
  }
  function header_symbol(line, symbol) {
    symbol = line
    sub(/^[[:space:]]*[[:xdigit:]]+[[:space:]]+</, "", symbol)
    sub(/>:[[:space:]]*$/, "", symbol)
    return symbol
  }
  function dispatch_jump_allowed(symbol) {
    if (schema == "redesign_v1")
      return symbol ~ /^ItchParser::dispatchMessage\(/
    return symbol ~ /^ItchParser::handleMessage\(/
  }
  function dispatch_jump_limit() {
    return schema == "redesign_v1" ? 1 : 2
  }
  {
    if (is_header($0))
      caller = header_symbol($0)
    lower = tolower($0)
    instruction = lower
    sub(/[[:space:]]+<.*$/, "", instruction)
    if (instruction ~ /[[:space:]]callq?[[:space:]].*\*/ ||
        lower ~ /[[:space:]]blr[[:space:]]+x[0-9]+/) {
      print
      next
    }
    if (instruction ~ /[[:space:]]jmpq?[[:space:]].*\*/ ||
        lower ~ /[[:space:]]br[[:space:]]+x[0-9]+/) {
      if (!dispatch_jump_allowed(caller)) {
        print
      } else {
        ++dispatch_jumps[caller]
        dispatch_jump_lines[caller] = dispatch_jump_lines[caller] $0 "\n"
      }
    }
  }
  END {
    for (caller in dispatch_jumps) {
      if (dispatch_jumps[caller] > dispatch_jump_limit())
        printf "%s", dispatch_jump_lines[caller]
    }
  }
' "${temporary_selected}")"
if [[ -n "${indirect_matches}" ]]; then
  printf '%s\n' "${indirect_matches}"
  die "indirect call or tail call in hot closure"
fi
lock_matches="$(awk '
  {
    instruction = tolower($0)
    sub(/[[:space:]]+<.*$/, "", instruction)
    if (instruction ~ /[[:space:]]lock([[:space:];]|$)/)
      print
  }
' "${temporary_selected}")"
if [[ -n "${lock_matches}" ]]; then
  printf '%s\n' "${lock_matches}"
  die "x86 lock-prefixed instruction in hot closure"
fi

if [[ -n "${REPORT}" ]]; then
  [[ ! -e "${REPORT}" ]] || die "report path already exists: ${REPORT}"
  cp "${temporary_selected}" "${REPORT}"
fi

if command -v sha256sum >/dev/null 2>&1; then
  binary_sha256="$(sha256sum "${BINARY}" | awk '{ print $1 }')"
elif command -v shasum >/dev/null 2>&1; then
  binary_sha256="$(shasum -a 256 "${BINARY}" | awk '{ print $1 }')"
else
  die "sha256sum or shasum is required"
fi

printf 'hot_path_verifier version=2 result=PASS schema=%s binary_sha256=%s selected_functions=%s forbidden_targets=0 indirect_calls=0 lock_prefixes=0' \
  "${SCHEMA}" "${binary_sha256}" "${selected_functions}"
if [[ -n "${REPORT}" ]]; then
  printf ' report=%s' "${REPORT}"
fi
printf '\n'
