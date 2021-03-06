//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "../../http_private.h"
#include "./web_socket_interceptor.h"
#include "./web_socket_datastructure.h"
#include "./web_socket_frame.h"

#include <base/ovcrypto/ovcrypto.h>
#include <http_server/http_server.h>
#include <utility>

WebSocketInterceptor::~WebSocketInterceptor()
{
}

bool WebSocketInterceptor::IsInterceptorForRequest(const std::shared_ptr<const HttpRequest> &request, const std::shared_ptr<const HttpResponse> &response)
{
	// 여기서 web socket request 인지 확인
	// RFC6455 - 4.2.1.  Reading the Client's Opening Handshake
	//
	// 1.   An HTTP/1.1 or higher GET request, including a "Request-URI"
	//      [RFC2616] that should be interpreted as a /resource name/
	//      defined in Section 3 (or an absolute HTTP/HTTPS URI containing
	//      the /resource name/).
	//
	// 2.   A |Host| header field containing the server's authority.
	//
	if((request->GetMethod() == HttpMethod::Get) && (request->GetHttpVersionAsNumber() > 1.0))
	{
		if(
			// 3.   An |Upgrade| header field containing the value "websocket",
			//      treated as an ASCII case-insensitive value.
			(request->GetHeader("UPGRADE") == "websocket") &&

			// 4.   A |Connection| header field that includes the token "Upgrade",
			//      treated as an ASCII case-insensitive value.
			(request->GetHeader("CONNECTION").UpperCaseString().IndexOf("UPGRADE") >= 0L) &&

			// 5.   A |Sec-WebSocket-Key| header field with a base64-encoded (see
			//      Section 4 of [RFC4648]) value that, when decoded, is 16 bytes in
			//      length.
			request->IsHeaderExists("SEC-WEBSOCKET-KEY") &&

			// 6.   A |Sec-WebSocket-Version| header field, with a value of 13.
			(request->GetHeader("SEC-WEBSOCKET-VERSION") == "13")
			)
		{
			// 나머지 사항은 체크하지 않음
			// 7.   Optionally, an |Origin| header field.  This header field is sent
			//      by all browser clients.  A connection attempt lacking this
			//      header field SHOULD NOT be interpreted as coming from a browser
			//      client.
			//
			// 8.   Optionally, a |Sec-WebSocket-Protocol| header field, with a list
			//      of values indicating which protocols the client would like to
			//      speak, ordered by preference.
			//
			// 9.   Optionally, a |Sec-WebSocket-Extensions| header field, with a
			//      list of values indicating which extensions the client would like
			//      to speak.  The interpretation of this header field is discussed
			//      in Section 9.1.
			//
			// 10.  Optionally, other header fields, such as those used to send
			//      cookies or request authentication to a server.  Unknown header
			//      fields are ignored, as per [RFC2616].

			logtd("%s is websocket request", request->ToString().CStr());

			return true;
		}
	}

	logtd("%s is not websocket request", request->ToString().CStr());

	return false;
}

bool WebSocketInterceptor::OnHttpPrepare(const std::shared_ptr<HttpRequest> &request, const std::shared_ptr<HttpResponse> &response)
{
	// RFC6455 - 4.2.2.  Sending the Server's Opening Handshake
	response->SetStatusCode(HttpStatusCode::SwitchingProtocols);

	response->SetHeader("Upgrade", "websocket");
	response->SetHeader("Connection", "Upgrade");

	// 4.  A |Sec-WebSocket-Accept| header field.  The value of this
	//    header field is constructed by concatenating /key/, defined
	//    above in step 4 in Section 4.2.2, with the string "258EAFA5-
	//    E914-47DA-95CA-C5AB0DC85B11", taking the SHA-1 hash of this
	//    concatenated value to obtain a 20-byte value and base64-
	//    encoding (see Section 4 of [RFC4648]) this 20-byte hash.
	const ov::String unique_id = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	ov::String key = request->GetHeader("SEC-WEBSOCKET-KEY");

	std::shared_ptr<ov::Data> hash = ov::MessageDigest::ComputeDigest(ov::CryptoAlgorithm::Sha1, (key + unique_id).ToData(false));
	ov::String base64 = ov::Base64::Encode(hash);

	response->SetHeader("Sec-WebSocket-Accept", base64);

	// client에 헤더 전송
	response->Response();

	// 지속적으로 통신해야 하므로, 연결은 끊지 않음
	logtd("Add to websocket client list: %s", request->ToString().CStr());
	auto websocket_response = std::make_shared<WebSocketClient>(response->GetRemote(), request, response);
	_websocket_client_list[request] = (WebSocketInfo){
		.response = websocket_response,
		.frame = nullptr
	};

	if(_connection_handler != nullptr)
	{
		return _connection_handler(websocket_response);
	}

	return true;
}

bool WebSocketInterceptor::OnHttpData(const std::shared_ptr<HttpRequest> &request, const std::shared_ptr<HttpResponse> &response, const std::shared_ptr<const ov::Data> &data)
{
	if(data->GetLength() == 0)
	{
		// Nothing to do
		return true;
	}

	auto item = _websocket_client_list.find(request);

	if(item == _websocket_client_list.end())
	{
		// 반드시 _websocket_client_list 목록 안에 있어야 함
		OV_ASSERT2(false);
		return false;
	}

	logtd("Data is received\n%s", data->Dump().CStr());

	if(item->second.frame == nullptr)
	{
		item->second.frame = std::make_shared<WebSocketFrame>();
	}

	auto frame = item->second.frame;
	auto processed_bytes = frame->Process(data);

	switch(frame->GetStatus())
	{
		case WebSocketFrameParseStatus::Prepare:
			// Not enough data to parse header
			break;

		case WebSocketFrameParseStatus::Parsing:
			break;

		case WebSocketFrameParseStatus::Completed:
		{
			const std::shared_ptr<const ov::Data> payload = frame->GetPayload();

			if(static_cast<WebSocketFrameOpcode>(frame->GetHeader().opcode) == WebSocketFrameOpcode::ConnectionClose)
			{
				// 접속 종료 요청됨
				logtd("Client requested close connection: reason:\n%s", payload->Dump("Reason").CStr());
				return false;
			}
			else
			{
				logtd("%s:\n%s", frame->ToString().CStr(), payload->Dump("Frame", 0L, 1024L, nullptr).CStr());

				// 패킷 조립이 완료되었음
				// 상위 레벨로 올림
				if(_message_handler != nullptr)
				{
					if(payload->GetLength() > 0L)
					{
						// 데이터가 있을 경우에만 올림
						if(_message_handler(item->second.response, frame) == false)
						{
							return false;
						}
					}

					item->second.frame = nullptr;
				}

				// 나머지 데이터로 다시 파싱 시작
				OV_ASSERT2(processed_bytes >= 0L);

				if(processed_bytes > 0L)
				{
					return OnHttpData(request, response, data->Subdata(processed_bytes));
				}
			}

			break;
		}

		case WebSocketFrameParseStatus::Error:
			// 잘못된 데이터가 수신되었음 WebSocket 연결을 해제함
			logtw("Invalid data received from %s", request->ToString().CStr());
			return false;
	}

	return true;
}

void WebSocketInterceptor::OnHttpError(const std::shared_ptr<HttpRequest> &request, const std::shared_ptr<HttpResponse> &response, HttpStatusCode status_code)
{
	auto item = _websocket_client_list.find(request);

	logtd("An error occurred: %s...", request->ToString().CStr());

	OV_ASSERT2(item != _websocket_client_list.end());

	if(_error_handler != nullptr)
	{
		_error_handler(item->second.response, ov::Error::CreateError(static_cast<int>(status_code), "%s", StringFromHttpStatusCode(status_code)));
	}

	_websocket_client_list.erase(item);

	response->SetStatusCode(status_code);
}

void WebSocketInterceptor::OnHttpClosed(const std::shared_ptr<HttpRequest> &request, const std::shared_ptr<HttpResponse> &response)
{
	auto item = _websocket_client_list.find(request);

	logtd("Deleting %s from websocket client list...", request->ToString().CStr());

	OV_ASSERT2(item != _websocket_client_list.end());

	if(_close_handler != nullptr)
	{
		_close_handler(item->second.response);
	}

	_websocket_client_list.erase(item);
}

void WebSocketInterceptor::SetConnectionHandler(WebSocketConnectionHandler handler)
{
	_connection_handler = std::move(handler);
}

void WebSocketInterceptor::SetMessageHandler(WebSocketMessageHandler handler)
{
	_message_handler = std::move(handler);
}

void WebSocketInterceptor::SetErrorHandler(WebSocketErrorHandler handler)
{
	_error_handler = std::move(handler);
}

void WebSocketInterceptor::SetCloseHandler(WebSocketCloseHandler handler)
{
	_close_handler = std::move(handler);
}
