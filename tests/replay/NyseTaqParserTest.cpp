#include "replay/NyseTaqParser.hpp"
#include "replay/SymbolTable.hpp"

#include <gtest/gtest.h>

TEST(NyseTaqParserTest, ParsesAddOrder) {
  SymbolTable symbols;
  NyseTaqParser parser(symbols, 2);
  MdEvent event{};

  ASSERT_TRUE(parser.parseLine(
      "100,123,09:30:00.123456789,AAPL,7,555,190.59,100,B", event));

  EXPECT_EQ(event.type, MdEventType::Add);
  EXPECT_EQ(event.channel_id, 2u);
  EXPECT_EQ(event.channel_seq_no, 123u);
  EXPECT_EQ(event.symbol_seq_no, 7u);
  EXPECT_EQ(event.symbol_id, 1u);
  EXPECT_EQ(event.order_id, 555u);
  EXPECT_EQ(event.price_ticks, 1905900);
  EXPECT_EQ(event.qty, 100u);
  EXPECT_EQ(event.side, 'B');
  EXPECT_EQ(event.ts_event_ns, 34200123456789ULL);
  ASSERT_NE(symbols.symbol(event.symbol_id), nullptr);
  EXPECT_EQ(*symbols.symbol(event.symbol_id), "AAPL");
}

TEST(NyseTaqParserTest, ParsesSubCentPriceAtFourDecimalScale) {
  int64_t price_ticks = 0;

  ASSERT_TRUE(NyseTaqParser::parsePriceTicks("0.125", price_ticks));

  EXPECT_EQ(price_ticks, 1250);
}

TEST(NyseTaqParserTest, ParsesLeadingDecimalPrice) {
  int64_t price_ticks = 0;

  ASSERT_TRUE(NyseTaqParser::parsePriceTicks(".83", price_ticks));

  EXPECT_EQ(price_ticks, 8300);
}

TEST(NyseTaqParserTest, ParsesModifyOrderSideAfterPositionChange) {
  SymbolTable symbols;
  NyseTaqParser parser(symbols);
  MdEvent event{};

  ASSERT_TRUE(parser.parseLine(
      "101,124,09:30:01.000000000,AAPL,8,555,190.595,50,0,S",
      event));

  EXPECT_EQ(event.type, MdEventType::Modify);
  EXPECT_EQ(event.price_ticks, 1905950);
  EXPECT_EQ(event.qty, 50u);
  EXPECT_EQ(event.side, 'S');
}

TEST(NyseTaqParserTest, ParsesReplaceOrder) {
  SymbolTable symbols;
  NyseTaqParser parser(symbols);
  MdEvent event{};

  ASSERT_TRUE(parser.parseLine(
      "104,125,09:30:02.000000000,AAPL,9,555,556,190.60,25,B",
      event));

  EXPECT_EQ(event.type, MdEventType::Replace);
  EXPECT_EQ(event.order_id, 555u);
  EXPECT_EQ(event.new_order_id, 556u);
  EXPECT_EQ(event.price_ticks, 1906000);
  EXPECT_EQ(event.qty, 25u);
  EXPECT_EQ(event.side, 'B');
}

TEST(NyseTaqParserTest, SkipsHeaderLines) {
  SymbolTable symbols;
  NyseTaqParser parser(symbols);
  MdEvent event{};

  EXPECT_FALSE(parser.parseLine("MsgType,SequenceNumber,SourceTime", event));
  EXPECT_TRUE(parser.lastLineSkipped());
  EXPECT_TRUE(parser.lastError().empty());
}
