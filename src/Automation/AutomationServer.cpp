#include <winsock2.h>
#include <ws2tcpip.h>
#include "AutomationServer.h"
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace Automation
{
	namespace
	{
		constexpr size_t MaxHeaders = 8192, MaxClients = 4;
		constexpr auto ClientTimeout = std::chrono::seconds(3);
		struct Client
		{
			SOCKET Socket;
			std::string Input, Output;
			size_t Sent = 0;
			bool Draining = false;
			std::shared_ptr<std::atomic<bool>> Cancel = std::make_shared<std::atomic<bool>>(false);
			std::future<HTTPResponse> Pending;
			std::chrono::steady_clock::time_point Deadline = std::chrono::steady_clock::now() + ClientTimeout;
		};
		std::string Wire(const HTTPResponse& response)
		{
			const auto body = response.Body.is_null() ? std::string() : response.Body.dump();
			std::string result = "HTTP/1.1 " + std::to_string(response.Status) + " Response\r\nConnection: close\r\nCache-Control: no-store\r\n";
			if (!body.empty()) result += "Content-Type: application/json\r\n";
			for (const auto& [name, value] : response.Headers) result += name + ": " + value + "\r\n";
			return result + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
		}
		// Bounded HTTP/1.1 profile: Content-Length, one request per connection.
		int Parse(const std::string& input, HTTPRequest& request)
		{
			const auto end = input.find("\r\n\r\n");
			if (end == std::string::npos) return input.size() > MaxHeaders ? 431 : 0;
			if (end > MaxHeaders) return 431;
			const auto first = input.find("\r\n");
			const auto space = input.find(' '), lastSpace = input.find(' ', space + 1);
			if (space == std::string::npos || lastSpace == std::string::npos || lastSpace > first
				|| input.substr(lastSpace + 1, first - lastSpace - 1) != "HTTP/1.1") return 400;
			request.Method = input.substr(0, space);
			if (input.substr(space + 1, lastSpace - space - 1) != "/mcp") return 404;
			for (size_t at = first + 2; at < end;)
			{
				const auto next = input.find("\r\n", at), colon = input.find(':', at);
				if (colon == std::string::npos || colon >= next || colon == at) return 400;
				std::string name = input.substr(at, colon - at);
				for (auto& c : name)
				{
					if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
					if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '-') return 400;
				}
				std::string value = input.substr(colon + 1, next - colon - 1);
				for (const unsigned char c : value) if ((c < 32 && c != '\t') || c > 126) return 400;
				const auto begin = value.find_first_not_of(" \t"), finish = value.find_last_not_of(" \t");
				value = begin == std::string::npos ? "" : value.substr(begin, finish - begin + 1);
				if (!request.Headers.emplace(std::move(name), std::move(value)).second) return 400;
				at = next + 2;
			}
			if (request.Headers.contains("transfer-encoding") || request.Headers.contains("expect")) return 400;
			size_t length = 0;
			if (const auto it = request.Headers.find("content-length"); it != request.Headers.end())
			{
				const auto& value = it->second;
				const auto parsed = std::from_chars(value.data(), value.data() + value.size(), length);
				if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size()) return 400;
			}
			else if (request.Method == "POST") return 411;
			if (length > MCPProtocol::MaxBodyBytes) return 413;
			if (input.size() < end + 4 + length) return 0;
			if (input.size() != end + 4 + length) return 400;
			request.Body = input.substr(end + 4, length);
			return 200;
		}
	}
	struct AutomationServer::Impl
	{
		SOCKET Listener = INVALID_SOCKET;
		std::unique_ptr<MCPProtocol> Protocol;
		std::thread IO;
		std::atomic<bool> Stopping {false};
		void Run()
		{
			std::vector<Client> clients;
			while (!Stopping.load())
			{
				fd_set read {}, write {};
				FD_SET(Listener, &read);
				for (const auto& c : clients)
				{
					if (c.Socket == INVALID_SOCKET) continue;
					if (c.Output.empty() || c.Draining) FD_SET(c.Socket, &read); else FD_SET(c.Socket, &write);
				}
				timeval wait {0, 20000};
				if (select(0, &read, &write, nullptr, &wait) == SOCKET_ERROR) break;
				if (FD_ISSET(Listener, &read))
				{
					SOCKET socket = accept(Listener, nullptr, nullptr);
					if (socket != INVALID_SOCKET)
					{
						u_long nonblocking = 1;
						if (clients.size() >= MaxClients || ioctlsocket(socket, FIONBIO, &nonblocking)) closesocket(socket);
						else clients.push_back(Client{socket, {}, {}});
					}
				}
				for (auto i = clients.begin(); i != clients.end();)
				{
					if (i->Pending.valid() && i->Pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
					{
						i->Output = Wire(i->Pending.get());
						i->Deadline = std::chrono::steady_clock::now() + ClientTimeout;
					}
					bool closed = i->Socket == INVALID_SOCKET || std::chrono::steady_clock::now() >= i->Deadline;
					if (!closed && FD_ISSET(i->Socket, &read))
					{
						std::array<char, 4096> buffer;
						const auto count = recv(i->Socket, buffer.data(), int(buffer.size()), 0);
						if (count > 0)
						{
							if (i->Pending.valid()) closed = true; // No HTTP pipelining.
							else if (!i->Draining)
							{
								i->Input.append(buffer.data(), count);
								HTTPRequest request;
								const auto parsed = Parse(i->Input, request);
								if (parsed == 200)
								{
									request.Cancel = i->Cancel;
									i->Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(32);
									i->Pending = std::async(std::launch::async, [this, request = std::move(request)] { return Protocol->Handle(request); });
									std::string().swap(i->Input);
								}
								else if (parsed) i->Output = Wire(HTTPResponse{parsed, nullptr, {}});
							}
						}
						else if (count == 0 || WSAGetLastError() != WSAEWOULDBLOCK) closed = true;
					}
					if (!closed && FD_ISSET(i->Socket, &write))
					{
						const auto sent = send(i->Socket, i->Output.data() + i->Sent,
							static_cast<int>(std::min<size_t>(65536, i->Output.size() - i->Sent)), 0);
						if (sent > 0)
						{
							i->Sent += sent;
							if (i->Sent == i->Output.size())
							{
								// Half-close and drain bounded input: closing with unread
								// oversized bodies can reset TCP before the 413 arrives.
								shutdown(i->Socket, SD_SEND);
								i->Draining = true;
								std::string().swap(i->Input);
							}
						}
						else if (sent == 0 || WSAGetLastError() != WSAEWOULDBLOCK) closed = true;
					}
					if (closed)
					{
						*i->Cancel = true;
						if (i->Socket != INVALID_SOCKET) closesocket(i->Socket);
						i->Socket = INVALID_SOCKET;
						// Retain the bounded slot until its worker releases the command.
						if (!i->Pending.valid()) { i = clients.erase(i); continue; }
					}
					++i;
				}
			}
			for (const auto& client : clients)
			{
				*client.Cancel = true;
				if (client.Socket != INVALID_SOCKET) closesocket(client.Socket);
			}
		}
	};
	AutomationServer::AutomationServer() = default;
	AutomationServer::~AutomationServer() { Stop(); }
	bool AutomationServer::Start(int port, const std::string& token, const std::string& instance, MCPProtocol::StatusReader reader,
		Json tools, MCPProtocol::ToolExecutor executor)
	{
		if (State || port < 1 || port > 65535 || token.size() < 32 || token.size() > 256) return false;
		WSADATA data;
		if (WSAStartup(MAKEWORD(2, 2), &data)) return false;
		State = std::make_unique<Impl>();
		State->Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		sockaddr_in address {};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(static_cast<u_short>(port));
		BOOL exclusive = TRUE;
		u_long nonblocking = 1;
		if (State->Listener == INVALID_SOCKET
			|| setsockopt(State->Listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive))
			|| bind(State->Listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address))
			|| listen(State->Listener, 16) || ioctlsocket(State->Listener, FIONBIO, &nonblocking))
		{ Stop(); return false; }
		State->Protocol = std::make_unique<MCPProtocol>(port, token, instance, std::move(reader));
		if (executor) State->Protocol->ConfigureTools(std::move(tools), std::move(executor));
		State->IO = std::thread([this] { State->Run(); });
		return true;
	}
	void AutomationServer::Stop()
	{
		if (!State) return;
		State->Stopping = true;
		if (State->IO.joinable()) State->IO.join();
		if (State->Listener != INVALID_SOCKET) closesocket(State->Listener);
		State.reset();
		WSACleanup();
	}
}
