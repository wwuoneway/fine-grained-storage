void inspect_columns(const char* ntupleName, const char* path)
{
  auto inspector = ROOT::Experimental::RNTupleInspector::Create(ntupleName, path);
  const auto& desc = inspector->GetDescriptor();

  for (const auto& col : desc.GetColumnIterable()) {
    if (col.IsAliasColumn())
      continue;
    const auto& colInfo = inspector->GetColumnInspector(col.GetPhysicalId());
    printf("col %2llu  field=%-30s pages=%-6llu elements=%-10llu compressed=%llu B\n",
           (unsigned long long)col.GetPhysicalId(),
           desc.GetQualifiedFieldName(col.GetFieldId()).c_str(),
           (unsigned long long)colInfo.GetNPages(),
           (unsigned long long)colInfo.GetNElements(),
           (unsigned long long)colInfo.GetCompressedSize());
  }
}
