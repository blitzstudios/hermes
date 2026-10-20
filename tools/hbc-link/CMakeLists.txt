add_hermes_tool(hbc-link
  hbc-link.cpp
  ${ALL_HEADER_FILES}
  )

target_link_libraries(hbc-link
  hermesHBCBackend
)
