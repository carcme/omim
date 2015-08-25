#include "query_saver.hpp"

#include "platform/settings.hpp"

#include "coding/base64.hpp"
#include "coding/reader.hpp"
#include "coding/writer.hpp"
#include "coding/write_to_sink.hpp"

#include "base/logging.hpp"

namespace
{
char constexpr kSettingsKey[] = "UserQueries";
using TLength = uint16_t;
TLength constexpr kMaxSuggestCount = 10;
size_t constexpr kLengthTypeSize = sizeof(TLength);

bool ReadLength(ReaderSource<MemReader> & reader, TLength & length)
{
  if (reader.Size() < kLengthTypeSize)
    return false;
  length = ReadPrimitiveFromSource<TLength>(reader);
  return true;
}
}  // namespace

namespace search
{
QuerySaver::QuerySaver()
{
  Load();
}

void QuerySaver::Add(string const & query)
{
  if (query.empty())
    return;

  // Remove items if needed.
  auto const it = find(m_topQueries.begin(), m_topQueries.end(), query);
  if (it != m_topQueries.end())
    m_topQueries.erase(it);
  else if (m_topQueries.size() >= kMaxSuggestCount)
    m_topQueries.pop_back();

  // Add new query and save it to drive.
  m_topQueries.push_front(query);
  Save();
}

void QuerySaver::Clear()
{
  m_topQueries.clear();
  Settings::Delete(kSettingsKey);
}

void QuerySaver::Serialize(string & data) const
{
  vector<uint8_t> rawData;
  MemWriter<vector<uint8_t>> writer(rawData);
  TLength size = m_topQueries.size();
  WriteToSink(writer, size);
  for (auto const & query : m_topQueries)
  {
    size = query.size();
    WriteToSink(writer, size);
    writer.Write(query.c_str(), size);
  }
  data = base64::Encode(string(rawData.begin(), rawData.end()));
}

void QuerySaver::EmergencyReset()
{
  Clear();
  LOG(LWARNING, ("Search history data corrupted! Creating new one."));
}

void QuerySaver::Deserialize(string const & data)
{
  string decodedData = base64::Decode(data);
  MemReader rawReader(decodedData.c_str(), decodedData.size());
  ReaderSource<MemReader> reader(rawReader);

  TLength queriesCount;
  if (!ReadLength(reader, queriesCount))
  {
    EmergencyReset();
    return;
  }

  queriesCount = min(queriesCount, kMaxSuggestCount);

  for (TLength i = 0; i < queriesCount; ++i)
  {
    TLength stringLength;
    if (!ReadLength(reader, stringLength))
    {
      EmergencyReset();
      return;
    }
    if (reader.Size() < stringLength)
    {
      EmergencyReset();
      return;
    }
    vector<char> str(stringLength);
    reader.Read(&str[0], stringLength);
    m_topQueries.emplace_back(&str[0], stringLength);
  }
}

void QuerySaver::Save()
{
  string data;
  Serialize(data);
  Settings::Set(kSettingsKey, data);
}

void QuerySaver::Load()
{
  string hexData;
  Settings::Get(kSettingsKey, hexData);
  if (hexData.empty())
    return;
  Deserialize(hexData);
}
}  // namesapce search
