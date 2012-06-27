cmake_minimum_required(VERSION 2.8.3)
include(PCH_GCC4_v2.cmake)
include_directories(
  ${CMAKE_CURRENT_LIST_DIR}
)

include_directories(
    ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../signalslot
    ../3rdparty/leveldb/include)

set(rtags_HDRS
    AbortInterface.h
    ByteArray.h
    Client.h
    Connection.h
    CursorInfo.h
    CursorInfoJob.h
    Database.h
    DirtyJob.h
    ErrorMessage.h
    FileSystemWatcher.h
    FindSymbolsJob.h
    FollowLocationJob.h
    GccArguments.h
    Indexer.h
    IndexerJob.h
    Job.h
    List.h
    ListSymbolsJob.h
    Location.h
    Log.h
    LogObject.h
    MakefileMessage.h
    MakefileParser.h
    Map.h
    MemoryMonitor.h
    Message.h
    Messages.h
    Mutex.h
    MutexLocker.h
    CreateOutputMessage.h
    Path.h
    Pch.h
    QueryMessage.h
    RTags.h
    Rdm.h
    ReadLocker.h
    ReadWriteLock.h
    ReferencesJob.h
    RegExp.h
    ResponseMessage.h
    RunTestJob.h
    SHA256.h
    ScopedDB.h
    Serializer.h
    Server.h
    Set.h
    Source.h
    StatusJob.h
    Str.h
    TestJob.h
    Thread.h
    ThreadPool.h
    WaitCondition.h
    WriteLocker.h
    EventLoop.h
    EventReceiver.h
    Event.h
    LocalClient.h
    LocalServer.h
    Process.h
    )

set(rtags_SRCS
    Client.cpp
    Connection.cpp
    CursorInfoJob.cpp
    Database.cpp
    DirtyJob.cpp
    ErrorMessage.cpp
    FileSystemWatcher.cpp
    FindSymbolsJob.cpp
    FollowLocationJob.cpp
    GccArguments.cpp
    Indexer.cpp
    IndexerJob.cpp
    Job.cpp
    ListSymbolsJob.cpp
    Location.cpp
    Log.cpp
    MakefileMessage.cpp
    MakefileParser.cpp
    MemoryMonitor.cpp
    Messages.cpp
    CreateOutputMessage.cpp
    Path.cpp
    QueryMessage.cpp
    RTags.cpp
    Rdm.cpp
    ReadWriteLock.cpp
    ReferencesJob.cpp
    RunTestJob.cpp
    SHA256.cpp
    ScopedDB.cpp
    Server.cpp
    StatusJob.cpp
    TestJob.cpp
    Thread.cpp
    ThreadPool.cpp
    EventLoop.cpp
    LocalClient.cpp
    LocalServer.cpp
    Process.cpp
)

include(clang.cmake)
include(PCH_GCC4_v2.cmake)

set(rtags_CPPMOCS
  ${CMAKE_CURRENT_LIST_DIR}/Connection.cpp
  )

add_pch_rule(Pch.h rtags_SRCS rtags_PCHFLAGS)
add_definitions(${rtags_PCHFLAGS})

add_custom_command(
    OUTPUT gccopts_gperf.h
    DEPENDS gccopts.gperf
    PRE_BUILD
    COMMAND gperf -I -C -l -L C++ gccopts.gperf -Z gccopts_gperf > gccopts_gperf.h
    VERBATIM
    )

add_custom_target(
    gperf
    DEPENDS gccopts_gperf.h
    )

add_library(rtags ${rtags_SRCS} ${MOCS})
add_dependencies(rtags gperf)