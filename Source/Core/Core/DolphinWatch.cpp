
#include <thread>
#include <atomic>
#include <Windows.h>

#include "DolphinWatch.h"
#include "Common/Logging/LogManager.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "InputCommon/InputConfig.h"
#include "Core/HW/Wiimote.h"
#include "Core/Core.h"
#include "Core/State.h"
#include "Common\StringUtil.h"

namespace DolphinWatch {

	using namespace std;

	static sf::TcpListener server;
	static vector<Client> clients;
	static char cbuf[1024];

	static thread thr;
	static atomic<bool> running=true;

	static int hijacks[NUM_WIIMOTES];

	WiimoteEmu::Wiimote* getWiimote(int i_wiimote) {
		return ((WiimoteEmu::Wiimote*)Wiimote::GetConfig()->controllers.at(i_wiimote));
	}

	void sendButtons(int i_wiimote, u16 _buttons) {
		if (!Core::IsRunning()) {
			// TODO error
			return;
		}
		WiimoteEmu::Wiimote* wiimote = getWiimote(i_wiimote);

		// disable reports from actual wiimote for a while, aka hijack for a while
		wiimote->SetReportingAuto(false);
		hijacks[i_wiimote] = HIJACK_TIMEOUT;

		u8 data[4];
		memset(data, 0, sizeof(data));

		data[0] = 0xA1; // input (wiimote -> wii)
		data[1] = 0x35; // mode: Core Buttons and Accelerometer with 16 Extension Bytes
			            // because just core buttons does not work for some reason.
		((wm_buttons*)(data + 2))->hex |= _buttons;
		
		// Just a suspicion, but maybe other threads could still be processing wiimote data?
		// This report shall be the newest, and not be overwritten, so yield once for safety
		this_thread::yield();

		Core::Callback_WiimoteInterruptChannel(i_wiimote, wiimote->GetReportingChannel(), data, 4);

	}

	void checkHijacks() {
		if (!Core::IsRunning() || Core::GetState() != Core::CORE_RUN) {
			return;
		}
		for (int i = 0; i < NUM_WIIMOTES; ++i) {
			if (hijacks[i] <= 0) continue;
			hijacks[i] -= WATCH_TIMEOUT;
			if (hijacks[i] <= 0) {
				hijacks[i] = 0;
				getWiimote(i)->SetReportingAuto(true);
			}
		}
	}

	void Init(unsigned short port) {
		server.listen(port);
		// avoid threads or complicated select()'s, just poll in update.
		server.setBlocking(false);

		memset(hijacks, 0, sizeof(hijacks));

		thr = thread([]() {
			while (running) {
				update();
				Sleep(WATCH_TIMEOUT);
				checkHijacks();
			}
		});
	}

	void Shutdown() {
		running = false;
		if (thr.joinable()) thr.join();
		// socket closing is implicit for sfml library during destruction
	}

	void process(Client &client, string &line) {
		// turn line into another stream
		istringstream parts(line);
		string cmd;

		NOTICE_LOG(CONSOLE, "PROCESSING %s", line.c_str());

		if (!(parts >> cmd)) {
			// no command, empty line, skip
			NOTICE_LOG(CONSOLE, "empty command line %s", line.c_str());
			return;
		}

		if (cmd == "WRITE") {

			if (!Memory::IsInitialized()) {
				NOTICE_LOG(CONSOLE, "PowerPC memory not initialized, can't execute command: %s", line.c_str());
				return;
			}

			u32 mode, addr, val;

			if (!(parts >> mode >> addr >> val)) {
				NOTICE_LOG(CONSOLE, "Invalid command line: %s", line.c_str());
				return;
			}

			// Parsing OK
			switch (mode) {
			case 8:
				PowerPC::HostWrite_U8(val, addr);
				break;
			case 16:
				PowerPC::HostWrite_U16(val, addr);
				break;
			case 32:
				PowerPC::HostWrite_U32(val, addr);
				break;
			default:
				NOTICE_LOG(CONSOLE, "Wrong mode for writing, 8/16/32 required as 1st parameter. Command: %s", line.c_str());
			}
		}
		else if (cmd == "READ") {

			if (!Memory::IsInitialized()) {
				NOTICE_LOG(CONSOLE, "PowerPC memory not initialized, can't execute command: %s", line.c_str());
				return;
			}

			u32 mode, addr, val;

			if (!(parts >> mode >> addr)) {
				NOTICE_LOG(CONSOLE, "Invalid command line: %s", line.c_str());
				return;
			}

			// Parsing OK
			switch (mode) {
			case 8:
				val = PowerPC::HostRead_U8(addr);
				break;
			case 16:
				val = PowerPC::HostRead_U16(addr);
				break;
			case 32:
				val = PowerPC::HostRead_U32(addr);
				break;
			default:
				NOTICE_LOG(CONSOLE, "Wrong mode for reading, 8/16/32 required as 1st parameter. Command: %s", line.c_str());
				return;
			}

			ostringstream message;
			message << "MEM " << addr << " " << val << endl;
			send(*client.socket, message.str());

		}
		else if (cmd == "SUBSCRIBE") {

			u32 mode, addr;

			if (!(parts >> mode >> addr)) {
				// no valid parameters, skip
				NOTICE_LOG(CONSOLE, "Invalid command line: %s", line.c_str());
				return;
			}

			// TODO handle overlapping subscribes etc. better. maybe by returning the mode again?

			for (auto &sub : client.subs) {
				if (sub.addr == addr) {
					return;
				}
			}

			if (mode == 8 || mode == 16 || mode == 32) {
				client.subs.push_back(Subscription(addr, mode));
			}
			else {
				NOTICE_LOG(CONSOLE, "Wrong mode for subscribing, 8/16/32 required as 1st parameter. Command: %s", line.c_str());
				return;
			}

		}
		else if (cmd == "SUBSCRIBE_MULTI") {

			u32 size, addr;

			if (!(parts >> size >> addr)) {
				// no valid parameters, skip
				NOTICE_LOG(CONSOLE, "Invalid command line: %s", line.c_str());
				return;
			}

			// TODO handle overlapping subscribes etc. better. maybe by returning the mode again?

			for (auto &sub : client.subsMulti) {
				if (sub.addr == addr) {
					return;
				}
			}

			client.subsMulti.push_back(SubscriptionMulti(addr, size));

		}
		else if (cmd == "UNSUBSCRIBE") {

			u32 addr;

			if (!(parts >> addr)) {
				// no valid parameters, skip
				NOTICE_LOG(CONSOLE, "Invalid command line: %s", line.c_str());
				return;
			}

			for (auto iter = client.subs.begin(); iter != client.subs.end(); ++iter) {
				if (iter->addr == addr) {
					client.subs.erase(iter);
					return;
				}
			}

		}
		else if (cmd == "UNSUBSCRIBE_MULTI") {

			u32 addr;

			if (!(parts >> addr)) {
				// no valid parameters, skip
				NOTICE_LOG(CONSOLE, "Invalid command line: %s", line.c_str());
				return;
			}

			for (auto iter = client.subsMulti.begin(); iter != client.subsMulti.end(); ++iter) {
				if (iter->addr == addr) {
					client.subsMulti.erase(iter);
					return;
				}
			}

		}
		else if (cmd == "BUTTONSTATES") {

			int i_wiimote;
			u16 states;
			
			if (!(parts >> i_wiimote >> states)) {
				// no valid parameters, skip
				NOTICE_LOG(CONSOLE, "Invalid command line: %s", line.c_str());
				return;
			}

			if (i_wiimote >= NUM_WIIMOTES) {
				NOTICE_LOG(CONSOLE, "Invalid wiimote number %d in: %s", i_wiimote, line.c_str());
				return;
			}

			sendButtons(i_wiimote, states);

		}
		else if (cmd == "PAUSE") {
			if (!Core::IsRunning()) {
				NOTICE_LOG(CONSOLE, "Core not running, can't pause: %s", line.c_str());
				return;
			}
			Core::SetState(Core::CORE_PAUSE);
		}
		else if (cmd == "RESUME") {
			if (!Core::IsRunning()) {
				NOTICE_LOG(CONSOLE, "Core not running, can't resume: %s", line.c_str());
				return;
			}
			Core::SetState(Core::CORE_RUN);
		}
		else if (cmd == "SAVE") {

			if (!Core::IsRunning()) {
				NOTICE_LOG(CONSOLE, "Core not running, can't save savestate: %s", line.c_str());
				return;
			}

			string file;
			getline(parts, file);
			file = StripSpaces(file);
			if (file.empty() || file.find_first_of(":?\"<> | ") != string::npos) {
				NOTICE_LOG(CONSOLE, "Invalid filename for saving savestate: %s", line.c_str());
				return;
			}

			State::SaveAs(file);

		}
		else if (cmd == "LOAD") {

			if (!Core::IsRunning()) {
				NOTICE_LOG(CONSOLE, "Core not running, can't load savestate: %s", line.c_str());
				return;
			}

			string file;
			getline(parts, file);
			file = StripSpaces(file);
			if (file.empty() || file.find_first_of(":?\"<> | ") != string::npos) {
				NOTICE_LOG(CONSOLE, "Invalid filename for loading savestate: %s", line.c_str());
				return;
			}

			State::LoadAs(file);

		}
		else {
			NOTICE_LOG(CONSOLE, "Unknown command: %s", cmd.c_str());
		}

	}

	void checkSubs(Client &client) {
		if (!Memory::IsInitialized()) return;
		for (auto &sub : client.subs) {
			u32 val;
			if (sub.mode == 8) val = PowerPC::HostRead_U8(sub.addr);
			else if (sub.mode == 16) val = PowerPC::HostRead_U16(sub.addr);
			else if (sub.mode == 32) val = PowerPC::HostRead_U32(sub.addr);
			if (val != sub.prev) {
				sub.prev = val;
				ostringstream message;
				message << "MEM " << sub.addr << " " << val << endl;
				send(*client.socket, message.str());
			}
		}
		for (auto &sub : client.subsMulti) {
			vector<u32> val(sub.size, 0);
			for (u32 i = 0; i < sub.size; ++i) {
				val.at(i) = PowerPC::HostRead_U8(sub.addr + i);
			}
			if (val != sub.prev) {
				sub.prev = val;
				ostringstream message;
				message << "MEM_MULTI " << sub.addr << " ";
				for (size_t i = 0; i < val.size(); ++i) {
					if (i != 0) message << " ";
					message << val.at(i);
				}
				message << endl;
				send(*client.socket, message.str());
			}
		}
	}

	void update() {

		string s;

		// poll for new clients, nonblocking
		auto socket = make_shared<sf::TcpSocket>();
		if (server.accept(*socket) == sf::Socket::Done) {
			socket->setBlocking(false);
			Client client(socket);
			clients.push_back(client);
		}

		// poll incoming data from clients, then process
		for (Client &client : clients) {

			// clean the client's buffer.
			// By default a stringbuffer keeps already read data.
			// a deque would do what we want, by not keeping that data, but then we would not have
			// access to nice stream-features like <</>> operators and getline(), so we do this manually.

			client.buf.clear(); // reset eol flag
			getline(client.buf, s, '\0'); // read everything
			client.buf.clear(); // reset eol flag again
			client.buf.str(""); // empty stringstream
			client.buf << s; // insert rest at beginning again

			size_t received = 0;
			auto status = client.socket->receive(cbuf, sizeof(cbuf) - 1, received);
			if (status == sf::Socket::Status::Disconnected || status == sf::Socket::Status::Error) {
				client.disconnected = true;
			}
			else if (status == sf::Socket::Status::Done) {
				// add nullterminator, then add to client's buffer
				cbuf[received] = '\0';
				client.buf << cbuf;

				// process the client's buffer
				size_t posg = 0;
				while (getline(client.buf, s)) {
					if (client.buf.eof()) {
						client.buf.clear();
						client.buf.seekg(posg);
						break;
					}
					posg = client.buf.tellg();

					// Might contain semicolons to further split several commands.
					// Doing that ensures that those commands are executed at once / in the same emulated frame.
					string s2;
					istringstream subcmds(s);
					while (getline(subcmds, s2, ';')) {
						if (!s2.empty()) process(client, s2);
					}
				}
			}

			// check subscriptions
			checkSubs(client);
		}

		// remove disconnected clients
		auto new_end = remove_if(clients.begin(), clients.end(), [](Client &c) {
			return c.disconnected;
		});
		clients.erase(new_end, clients.end());
	}

	void send(sf::TcpSocket &socket, string &message) {
		socket.setBlocking(true);
		socket.send(message.c_str(), message.size());
		socket.setBlocking(false);
	}
}
