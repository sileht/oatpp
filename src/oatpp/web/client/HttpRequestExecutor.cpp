/***************************************************************************
 *
 * Project         _____    __   ____   _      _
 *                (  _  )  /__\ (_  _)_| |_  _| |_
 *                 )(_)(  /(__)\  )( (_   _)(_   _)
 *                (_____)(__)(__)(__)  |_|    |_|
 *
 *
 * Copyright 2018-present, Leonid Stryzhevskyi <lganzzzo@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************************/

#include "HttpRequestExecutor.hpp"

#include "oatpp/web/protocol/http/incoming/ResponseHeadersReader.hpp"
#include "oatpp/web/protocol/http/outgoing/Request.hpp"
#include "oatpp/web/protocol/http/outgoing/BufferBody.hpp"

#include "oatpp/network/tcp/Connection.hpp"
#include "oatpp/core/data/stream/ChunkedBuffer.hpp"
#include "oatpp/core/data/stream/StreamBufferedProxy.hpp"

#include <stdlib.h>
#include <sys/types.h>
#if defined(WIN32) || defined(_WIN32)
#include <io.h>
#else
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

namespace oatpp { namespace web { namespace client {

HttpRequestExecutor::HttpRequestExecutor(const std::shared_ptr<ClientConnectionProvider>& connectionProvider,
                                         const std::shared_ptr<RetryPolicy>& retryPolicy,
                                         const std::shared_ptr<const BodyDecoder>& bodyDecoder)
  : RequestExecutor(retryPolicy)
  , m_connectionProvider(connectionProvider)
  , m_bodyDecoder(bodyDecoder)
{}

std::shared_ptr<HttpRequestExecutor>
HttpRequestExecutor::createShared(const std::shared_ptr<ClientConnectionProvider>& connectionProvider,
                                  const std::shared_ptr<RetryPolicy>& retryPolicy,
                                  const std::shared_ptr<const BodyDecoder>& bodyDecoder)
{
  return std::make_shared<HttpRequestExecutor>(connectionProvider, retryPolicy, bodyDecoder);
}

std::shared_ptr<HttpRequestExecutor::ConnectionHandle> HttpRequestExecutor::getConnection() {
  auto connection = m_connectionProvider->get();
  if(!connection){
    throw RequestExecutionError(RequestExecutionError::ERROR_CODE_CANT_CONNECT,
                                "[oatpp::web::client::HttpRequestExecutor::getConnection()]: ConnectionProvider failed to provide Connection");
  }
  return std::make_shared<HttpConnectionHandle>(connection);
}

oatpp::async::CoroutineStarterForResult<const std::shared_ptr<HttpRequestExecutor::ConnectionHandle>&>
HttpRequestExecutor::getConnectionAsync() {
  
  class GetConnectionCoroutine : public oatpp::async::CoroutineWithResult<GetConnectionCoroutine, const std::shared_ptr<ConnectionHandle>&> {
  private:
    std::shared_ptr<ClientConnectionProvider> m_connectionProvider;
  public:
    
    GetConnectionCoroutine(const std::shared_ptr<ClientConnectionProvider>& connectionProvider)
      : m_connectionProvider(connectionProvider)
    {}
    
    Action act() override {
      return m_connectionProvider->getAsync().callbackTo(&GetConnectionCoroutine::onConnectionReady);
    }
    
    Action onConnectionReady(const std::shared_ptr<oatpp::data::stream::IOStream>& connection) {
      return _return(std::make_shared<HttpConnectionHandle>(connection));
    }
    
  };
  
  return GetConnectionCoroutine::startForResult(m_connectionProvider);
  
}

void HttpRequestExecutor::invalidateConnection(const std::shared_ptr<ConnectionHandle>& connectionHandle) {

  if(connectionHandle) {
    auto connection = static_cast<HttpConnectionHandle*>(connectionHandle.get())->connection;
    m_connectionProvider->invalidate(connection);
  }

}
  
std::shared_ptr<HttpRequestExecutor::Response>
HttpRequestExecutor::executeOnce(const String& method,
                                 const String& path,
                                 const Headers& headers,
                                 const std::shared_ptr<Body>& body,
                                 const std::shared_ptr<ConnectionHandle>& connectionHandle) {
  
  std::shared_ptr<oatpp::data::stream::IOStream> connection;
  if(connectionHandle) {
    connection = static_cast<HttpConnectionHandle*>(connectionHandle.get())->connection;
  }

  if(!connection){
    throw RequestExecutionError(RequestExecutionError::ERROR_CODE_CANT_CONNECT,
                                "[oatpp::web::client::HttpRequestExecutor::executeOnce()]: Connection is null");
  }

  connection->setInputStreamIOMode(data::stream::IOMode::BLOCKING);
  connection->setOutputStreamIOMode(data::stream::IOMode::BLOCKING);
  
  auto request = oatpp::web::protocol::http::outgoing::Request::createShared(method, path, headers, body);
  request->putHeaderIfNotExists_Unsafe(oatpp::web::protocol::http::Header::HOST, m_connectionProvider->getProperty("host"));
  request->putHeaderIfNotExists_Unsafe(oatpp::web::protocol::http::Header::CONNECTION, oatpp::web::protocol::http::Header::Value::CONNECTION_KEEP_ALIVE);

  oatpp::data::share::MemoryLabel buffer(oatpp::base::StrBuffer::createShared(oatpp::data::buffer::IOBuffer::BUFFER_SIZE));

  oatpp::data::stream::OutputStreamBufferedProxy upStream(connection, buffer);
  request->send(&upStream);
  upStream.flush();
  
  oatpp::web::protocol::http::incoming::ResponseHeadersReader headerReader(buffer, 4096);
  oatpp::web::protocol::http::HttpError::Info error;
  const auto& result = headerReader.readHeaders(connection, error);
  
  if(error.status.code != 0) {
    invalidateConnection(connectionHandle);
    throw RequestExecutionError(RequestExecutionError::ERROR_CODE_CANT_PARSE_STARTING_LINE,
                                "[oatpp::web::client::HttpRequestExecutor::executeOnce()]: Failed to parse response. Invalid response headers");
  }
  
  if(error.ioStatus < 0) {
    invalidateConnection(connectionHandle);
    throw RequestExecutionError(RequestExecutionError::ERROR_CODE_CANT_PARSE_STARTING_LINE,
                                "[oatpp::web::client::HttpRequestExecutor::executeOnce()]: Failed to read response.");
  }
  
  auto bodyStream = oatpp::data::stream::InputStreamBufferedProxy::createShared(connection,
                                                                                buffer,
                                                                                result.bufferPosStart,
                                                                                result.bufferPosEnd,
                                                                                result.bufferPosStart != result.bufferPosEnd);
  
  return Response::createShared(result.startingLine.statusCode,
                                result.startingLine.description.toString(),
                                result.headers, bodyStream, m_bodyDecoder);
  
}

oatpp::async::CoroutineStarterForResult<const std::shared_ptr<HttpRequestExecutor::Response>&>
HttpRequestExecutor::executeOnceAsync(const String& method,
                                      const String& path,
                                      const Headers& headers,
                                      const std::shared_ptr<Body>& body,
                                      const std::shared_ptr<ConnectionHandle>& connectionHandle) {
  
  typedef protocol::http::incoming::ResponseHeadersReader ResponseHeadersReader;
  
  class ExecutorCoroutine : public oatpp::async::CoroutineWithResult<ExecutorCoroutine, const std::shared_ptr<HttpRequestExecutor::Response>&> {
  private:
    typedef oatpp::web::protocol::http::outgoing::Request OutgoingRequest;
  private:
    HttpRequestExecutor* m_this;
    String m_method;
    String m_path;
    Headers m_headers;
    std::shared_ptr<Body> m_body;
    std::shared_ptr<const BodyDecoder> m_bodyDecoder;
    std::shared_ptr<ConnectionHandle> m_connectionHandle;
    oatpp::data::share::MemoryLabel m_buffer;
    ResponseHeadersReader m_headersReader;
    std::shared_ptr<oatpp::data::stream::OutputStreamBufferedProxy> m_upstream;
  private:
    std::shared_ptr<oatpp::data::stream::IOStream> m_connection;
  public:
    
    ExecutorCoroutine(HttpRequestExecutor* _this,
                      const String& method,
                      const String& path,
                      const Headers& headers,
                      const std::shared_ptr<Body>& body,
                      const std::shared_ptr<const BodyDecoder>& bodyDecoder,
                      const std::shared_ptr<ConnectionHandle>& connectionHandle)
      : m_this(_this)
      , m_method(method)
      , m_path(path)
      , m_headers(headers)
      , m_body(body)
      , m_bodyDecoder(bodyDecoder)
      , m_connectionHandle(connectionHandle)
      , m_buffer(base::StrBuffer::createShared(oatpp::data::buffer::IOBuffer::BUFFER_SIZE))
      , m_headersReader(m_buffer, 4096)
    {}
    
    Action act() override {

      if(m_connectionHandle) {
        m_connection = static_cast<HttpConnectionHandle*>(m_connectionHandle.get())->connection;
      }

      if(!m_connection) {
        throw RequestExecutionError(RequestExecutionError::ERROR_CODE_CANT_CONNECT,
                                    "[oatpp::web::client::HttpRequestExecutor::executeOnceAsync::ExecutorCoroutine{act()}]: Connection is null");
      }

      m_connection->setInputStreamIOMode(data::stream::IOMode::ASYNCHRONOUS);
      m_connection->setOutputStreamIOMode(data::stream::IOMode::ASYNCHRONOUS);

      auto request = OutgoingRequest::createShared(m_method, m_path, m_headers, m_body);
      request->putHeaderIfNotExists_Unsafe(Header::HOST, m_this->m_connectionProvider->getProperty("host"));
      request->putHeaderIfNotExists_Unsafe(Header::CONNECTION, Header::Value::CONNECTION_KEEP_ALIVE);
      m_upstream = oatpp::data::stream::OutputStreamBufferedProxy::createShared(m_connection, m_buffer);
      return OutgoingRequest::sendAsync(request, m_upstream).next(m_upstream->flushAsync()).next(yieldTo(&ExecutorCoroutine::readResponse));

    }
    
    Action readResponse() {
      return m_headersReader.readHeadersAsync(m_connection).callbackTo(&ExecutorCoroutine::onHeadersParsed);
    }
    
    Action onHeadersParsed(const ResponseHeadersReader::Result& result) {
      
      auto bodyStream = oatpp::data::stream::InputStreamBufferedProxy::createShared(m_connection,
                                                                                    m_buffer,
                                                                                    result.bufferPosStart,
                                                                                    result.bufferPosEnd,
                                                                                    result.bufferPosStart != result.bufferPosEnd);
      
      return _return(Response::createShared(result.startingLine.statusCode,
                                            result.startingLine.description.toString(),
                                            result.headers, bodyStream, m_bodyDecoder));
      
    }

    Action handleError(oatpp::async::Error* error) override {

      if(m_connectionHandle) {
        m_this->invalidateConnection(m_connectionHandle);
      }

      return error;

    }
    
  };
  
  return ExecutorCoroutine::startForResult(this, method, path, headers, body, m_bodyDecoder, connectionHandle);
  
}
  
}}}
