
#include <iostream>
#include <boost/timer/timer.hpp>
#include <boost/tokenizer.hpp>

//configuration
#define ST_ASIO_SERVER_PORT		9527
//#define ST_ASIO_REUSE_OBJECT //use objects pool
#define ST_ASIO_DELAY_CLOSE		5 //define this to avoid hooks for async call (and slightly improve efficiency)
//#define ST_ASIO_FORCE_TO_USE_MSG_RECV_BUFFER //force to use the msg recv buffer
//#define ST_ASIO_CLEAR_OBJECT_INTERVAL 1
//#define ST_ASIO_WANT_MSG_SEND_NOTIFY
#define ST_ASIO_FULL_STATISTIC //full statistic will slightly impact efficiency
//#define ST_ASIO_MAX_MSG_NUM		16
//if there's a huge number of links, please reduce messge buffer via ST_ASIO_MAX_MSG_NUM macro.
//please think about if we have 512 links, how much memory we can accupy at most with default ST_ASIO_MAX_MSG_NUM?
//it's 2 * 1024 * 1024 * 512 = 1G

//use the following macro to control the type of packer and unpacker
#define PACKER_UNPACKER_TYPE	0
//0-default packer and unpacker, head(length) + body
//1-replaceable packer and unpacker, head(length) + body
//2-fixed length packer and unpacker
//3-prefix and/or suffix packer and unpacker

#if 1 == PACKER_UNPACKER_TYPE
#define ST_ASIO_DEFAULT_PACKER replaceable_packer<>
#define ST_ASIO_DEFAULT_UNPACKER replaceable_unpacker<>
#elif 2 == PACKER_UNPACKER_TYPE
#undef ST_ASIO_HEARTBEAT_INTERVAL
#define ST_ASIO_HEARTBEAT_INTERVAL	0 //not support heartbeat
#define ST_ASIO_DEFAULT_PACKER fixed_length_packer
#define ST_ASIO_DEFAULT_UNPACKER fixed_length_unpacker
#elif 3 == PACKER_UNPACKER_TYPE
#undef ST_ASIO_HEARTBEAT_INTERVAL
#define ST_ASIO_HEARTBEAT_INTERVAL	0 //not support heartbeat
#define ST_ASIO_DEFAULT_PACKER prefix_suffix_packer
#define ST_ASIO_DEFAULT_UNPACKER prefix_suffix_unpacker
#endif

#if defined(ST_ASIO_WANT_MSG_SEND_NOTIFY) && (!defined(ST_ASIO_HEARTBEAT_INTERVAL) || ST_ASIO_HEARTBEAT_INTERVAL <= 0)
#define ST_ASIO_INPUT_QUEUE non_lock_queue //we will never operate sending buffer concurrently, so need no locks.
#endif
//configuration

#include "../include/ext/st_asio_wrapper_client.h"
using namespace st_asio_wrapper;
using namespace st_asio_wrapper::ext;

#ifdef _MSC_VER
#define atoll _atoi64
#endif

#define QUIT_COMMAND	"quit"
#define RESTART_COMMAND	"restart"
#define LIST_ALL_CLIENT	"list_all_client"
#define LIST_STATUS		"status"

static bool check_msg;

//about congestion control
//
//in 1.3, congestion control has been removed (no post_msg nor post_native_msg anymore), this is because
//without known the business (or logic), framework cannot always do congestion control properly.
//now, users should take the responsibility to do congestion control, there're two ways:
//
//1. for receiver, if you cannot handle msgs timely, which means the bottleneck is in your business,
//    you should open/close congestion control intermittently;
//   for sender, send msgs in on_msg_send() or use sending buffer limitation (like safe_send_msg(..., false)),
//    but must not in service threads, please note.
//
//2. for sender, if responses are available (like pingpong test), send msgs in on_msg()/on_msg_handle(),
//    but this will reduce IO throughput because SOCKET's sliding window is not fully used, pleae note.
//
//test_client chose method #1

///////////////////////////////////////////////////
//msg sending interface
#define TCP_RANDOM_SEND_MSG(FUNNAME, SEND_FUNNAME) \
void FUNNAME(const char* const pstr[], const size_t len[], size_t num, bool can_overflow = false) \
{ \
	auto index = (size_t) ((uint64_t) rand() * (size() - 1) / RAND_MAX); \
	at(index)->SEND_FUNNAME(pstr, len, num, can_overflow); \
} \
TCP_SEND_MSG_CALL_SWITCH(FUNNAME, void)
//msg sending interface
///////////////////////////////////////////////////

class test_socket : public st_connector
{
public:
	test_socket(boost::asio::io_service& io_service_) : st_connector(io_service_), recv_bytes(0), recv_index(0)
	{
#if 2 == PACKER_UNPACKER_TYPE
		boost::dynamic_pointer_cast<ST_ASIO_DEFAULT_UNPACKER>(unpacker())->fixed_length(1024);
#elif 3 == PACKER_UNPACKER_TYPE
		boost::dynamic_pointer_cast<ST_ASIO_DEFAULT_PACKER>(packer())->prefix_suffix("begin", "end");
		boost::dynamic_pointer_cast<ST_ASIO_DEFAULT_UNPACKER>(unpacker())->prefix_suffix("begin", "end");
#endif
	}

	uint64_t get_recv_bytes() const {return recv_bytes;}
	operator uint64_t() const {return recv_bytes;}

	void clear_status() {recv_bytes = recv_index = 0;}
	void begin(size_t msg_num_, size_t msg_len, char msg_fill)
	{
		clear_status();
		msg_num = msg_num_;

		auto buff = new char[msg_len];
		memset(buff, msg_fill, msg_len);
		memcpy(buff, &recv_index, sizeof(size_t)); //seq

		send_msg(buff, msg_len);
		delete[] buff;
	}

protected:
	//msg handling
#ifndef ST_ASIO_FORCE_TO_USE_MSG_RECV_BUFFER
	//this virtual function doesn't exists if ST_ASIO_FORCE_TO_USE_MSG_RECV_BUFFER been defined
	virtual bool on_msg(out_msg_type& msg) {handle_msg(msg); return true;}
#endif
	virtual bool on_msg_handle(out_msg_type& msg) {handle_msg(msg); return true;}
	//msg handling end

#ifdef ST_ASIO_WANT_MSG_SEND_NOTIFY
	//congestion control, method #1, the peer needs its own congestion control too.
	virtual void on_msg_send(in_msg_type& msg)
	{
		if (0 == --msg_num)
			return;

		auto pstr = packer()->raw_data(msg);
		auto msg_len = packer()->raw_data_len(msg);

		size_t send_index;
		memcpy(&send_index, pstr, sizeof(size_t));
		++send_index;
		memcpy(pstr, &send_index, sizeof(size_t)); //seq

		send_msg(pstr, msg_len, true);
	}
#endif

private:
	void handle_msg(out_msg_ctype& msg)
	{
		recv_bytes += msg.size();
		if (check_msg && (msg.size() < sizeof(size_t) || 0 != memcmp(&recv_index, msg.data(), sizeof(size_t))))
			printf("check msg error: " ST_ASIO_SF ".\n", recv_index);
		++recv_index;

		//i'm the bottleneck -_-
//		boost::this_thread::sleep(boost::get_system_time() + boost::posix_time::milliseconds(10));
	}

private:
	uint64_t recv_bytes;
	size_t recv_index, msg_num;
};

class test_client : public st_tcp_client_base<test_socket>
{
public:
	test_client(st_service_pump& service_pump_) : st_tcp_client_base<test_socket>(service_pump_) {}

	uint64_t get_recv_bytes()
	{
		uint64_t total_recv_bytes = 0;
		do_something_to_all([&total_recv_bytes](object_ctype& item) {total_recv_bytes += *item;});
//		do_something_to_all([&total_recv_bytes](object_ctype& item) {total_recv_bytes += item->get_recv_bytes();});

		return total_recv_bytes;
	}

	statistic get_statistic()
	{
		statistic stat;
		do_something_to_all([&stat](object_ctype& item) {stat += item->get_statistic();});

		return stat;
	}

	void clear_status() {do_something_to_all([](object_ctype& item) {item->clear_status();});}
	void begin(size_t msg_num, size_t msg_len, char msg_fill) {do_something_to_all([=](object_ctype& item) {item->begin(msg_num, msg_len, msg_fill);});}

	void shutdown_some_client(size_t n)
	{
		static auto index = -1;
		++index;

		switch (index % 6)
		{
#ifdef ST_ASIO_CLEAR_OBJECT_INTERVAL
			//method #1
			//notice: these methods need to define ST_ASIO_CLEAR_OBJECT_INTERVAL macro, because it just shut down the st_socket,
			//not really remove them from object pool, this will cause test_client still send data via them, and wait responses from them.
			//for this scenario, the smaller ST_ASIO_CLEAR_OBJECT_INTERVAL macro is, the better experience you will get, so set it to 1 second.
		case 0: do_something_to_one([&n](object_ctype& item) {return n-- > 0 ? item->graceful_shutdown(), false : true;});				break;
		case 1: do_something_to_one([&n](object_ctype& item) {return n-- > 0 ? item->graceful_shutdown(false, false), false : true;});	break;
		case 2: do_something_to_one([&n](object_ctype& item) {return n-- > 0 ? item->force_shutdown(), false : true;});					break;
#else
			//method #2
			//this is a equivalence of calling i_server::del_socket in st_server_socket_base::on_recv_error(see st_server_socket_base for more details).
		case 0: while (n-- > 0) graceful_shutdown(at(0));			break;
		case 1: while (n-- > 0) graceful_shutdown(at(0), false);	break;
		case 2: while (n-- > 0) force_shutdown(at(0));				break;
#endif
			//if you just want to reconnect to the server, you should do it like this:
		case 3: do_something_to_one([&n](object_ctype& item) {return n-- > 0 ? item->graceful_shutdown(true), false : true;});			break;
		case 4: do_something_to_one([&n](object_ctype& item) {return n-- > 0 ? item->graceful_shutdown(true, false), false : true;});	break;
		case 5: do_something_to_one([&n](object_ctype& item) {return n-- > 0 ? item->force_shutdown(true), false : true;});				break;
		}
	}

	///////////////////////////////////////////////////
	//msg sending interface
	//guarantee send msg successfully even if can_overflow is false, success at here just means putting the msg into st_tcp_socket's send buffer successfully
	TCP_RANDOM_SEND_MSG(safe_random_send_msg, safe_send_msg)
	TCP_RANDOM_SEND_MSG(safe_random_send_native_msg, safe_send_native_msg)
	//msg sending interface
	///////////////////////////////////////////////////
};

void send_msg_one_by_one(test_client& client, size_t msg_num, size_t msg_len, char msg_fill)
{
	check_msg = true;
	uint64_t total_msg_bytes = msg_num * msg_len * client.size();

	boost::timer::cpu_timer begin_time;
	client.begin(msg_num, msg_len, msg_fill);
	unsigned percent = 0;
	do
	{
		boost::this_thread::sleep(boost::get_system_time() + boost::posix_time::milliseconds(50));

		auto new_percent = (unsigned) (100 * client.get_recv_bytes() / total_msg_bytes);
		if (percent != new_percent)
		{
			percent = new_percent;
			printf("\r%u%%", percent);
			fflush(stdout);
		}
	} while (percent < 100);
	begin_time.stop();

	auto used_time = (double) begin_time.elapsed().wall / 1000000000;
	printf(" finished in %f seconds, speed: %f(*2) MBps.\n", used_time, total_msg_bytes / used_time / 1024 / 1024);
}

void send_msg_randomly(test_client& client, size_t msg_num, size_t msg_len, char msg_fill)
{
	check_msg = false;
	uint64_t send_bytes = 0;
	uint64_t total_msg_bytes = msg_num * msg_len;
	auto buff = new char[msg_len];
	memset(buff, msg_fill, msg_len);

	boost::timer::cpu_timer begin_time;
	unsigned percent = 0;
	for (size_t i = 0; i < msg_num; ++i)
	{
		memcpy(buff, &i, sizeof(size_t)); //seq

		//congestion control, method #1, the peer needs its own congestion control too.
		client.safe_random_send_msg(buff, msg_len); //can_overflow is false, it's important
		send_bytes += msg_len;

		auto new_percent = (unsigned) (100 * send_bytes / total_msg_bytes);
		if (percent != new_percent)
		{
			percent = new_percent;
			printf("\r%u%%", percent);
			fflush(stdout);
		}
	}

	while(client.get_recv_bytes() < total_msg_bytes)
		boost::this_thread::sleep(boost::get_system_time() + boost::posix_time::milliseconds(50));

	begin_time.stop();
	delete[] buff;

	auto used_time = (double) begin_time.elapsed().wall / 1000000000;
	printf(" finished in %f seconds, speed: %f(*2) MBps.\n", used_time, total_msg_bytes / used_time / 1024 / 1024);
}

//use up to a specific worker threads to send messages concurrently
void send_msg_concurrently(test_client& client, size_t send_thread_num, size_t msg_num, size_t msg_len, char msg_fill)
{
	check_msg = true;
	auto link_num = client.size();
	auto group_num = std::min(send_thread_num, link_num);
	auto group_link_num = link_num / group_num;
	auto left_link_num = link_num - group_num * group_link_num;
	uint64_t total_msg_bytes = link_num * msg_len;
	total_msg_bytes *= msg_num;

	auto group_index = (size_t) -1;
	size_t this_group_link_num = 0;

	std::vector<std::list<test_client::object_type>> link_groups(group_num);
	client.do_something_to_all([&](test_client::object_ctype& item) {
		if (0 == this_group_link_num)
		{
			this_group_link_num = group_link_num;
			if (left_link_num > 0)
			{
				++this_group_link_num;
				--left_link_num;
			}

			++group_index;
		}

		--this_group_link_num;
		link_groups[group_index].emplace_back(item);
	});

	boost::timer::cpu_timer begin_time;
	boost::thread_group threads;
	do_something_to_all(link_groups, [&threads, msg_num, msg_len, msg_fill](const std::list<test_client::object_type>& item) {
		threads.create_thread([&item, msg_num, msg_len, msg_fill]() {
			auto buff = new char[msg_len];
			memset(buff, msg_fill, msg_len);
			for (size_t i = 0; i < msg_num; ++i)
			{
				memcpy(buff, &i, sizeof(size_t)); //seq

				//congestion control, method #1, the peer needs its own congestion control too.
				do_something_to_all(item, [buff, msg_len](test_client::object_ctype& item2) {item2->safe_send_msg(buff, msg_len);}); //can_overflow is false, it's important
			}
			delete[] buff;
		});
	});

	unsigned percent = 0;
	do
	{
		boost::this_thread::sleep(boost::get_system_time() + boost::posix_time::milliseconds(50));

		auto new_percent = (unsigned) (100 * client.get_recv_bytes() / total_msg_bytes);
		if (percent != new_percent)
		{
			percent = new_percent;
			printf("\r%u%%", percent);
			fflush(stdout);
		}
	} while (percent < 100);
	threads.join_all();
	begin_time.stop();

	auto used_time = (double) begin_time.elapsed().wall / 1000000000;
	printf(" finished in %f seconds, speed: %f(*2) MBps.\n", used_time, total_msg_bytes / used_time / 1024 / 1024);
}

static bool is_testing;
void start_test(int repeat_times, char mode, test_client& client, size_t send_thread_num, size_t msg_num, size_t msg_len, char msg_fill)
{
	for (int i = 0; i < repeat_times; ++i)
	{
		printf("this is the %d / %d test...\n", i + 1, repeat_times);
		client.clear_status();
#ifdef ST_ASIO_WANT_MSG_SEND_NOTIFY
		if (0 == mode)
			send_msg_one_by_one(client, msg_num, msg_len, msg_fill);
		else
		{
			puts("if ST_ASIO_WANT_MSG_SEND_NOTIFY defined, only support mode 0!");
			break;
		}
#else
		if (0 == mode)
			send_msg_concurrently(client, send_thread_num, msg_num, msg_len, msg_fill);
		else
			send_msg_randomly(client, msg_num, msg_len, msg_fill);
#endif
	}

	is_testing = false;
}

int main(int argc, const char* argv[])
{
	printf("usage: %s [<service thread number=1> [<send thread number=8> [<port=%d> [<ip=%s> [link num=16]]]]]\n", argv[0], ST_ASIO_SERVER_PORT, ST_ASIO_SERVER_IP);
	if (argc >= 2 && (0 == strcmp(argv[1], "--help") || 0 == strcmp(argv[1], "-h")))
		return 0;
	else
		puts("type " QUIT_COMMAND " to end.");

	///////////////////////////////////////////////////////////
	size_t link_num = 16;
	if (argc > 5)
		link_num = std::min(ST_ASIO_MAX_OBJECT_NUM, std::max(atoi(argv[5]), 1));

	printf("exec: %s with " ST_ASIO_SF " links\n", argv[0], link_num);
	///////////////////////////////////////////////////////////

	st_service_pump sp;
	test_client client(sp);
	//test_client means to cooperate with echo server while doing performance test, it will not send msgs back as echo server does,
	//otherwise, dead loop will occur, network resource will be exhausted.

//	argv[4] = "::1" //ipv6
//	argv[4] = "127.0.0.1" //ipv4
	std::string ip = argc > 4 ? argv[4] : ST_ASIO_SERVER_IP;
	unsigned short port = argc > 3 ? atoi(argv[3]) : ST_ASIO_SERVER_PORT;

	//method #1, create and add clients manually.
	auto socket_ptr = client.create_object();
	//socket_ptr->set_server_addr(port, ip); //we don't have to set server address at here, the following do_something_to_all will do it for us
	//some other initializations according to your business
	client.add_socket(socket_ptr, false);
	socket_ptr.reset(); //important, otherwise, st_object_pool will not be able to free or reuse this object.

	//method #2, add clients first without any arguments, then set the server address.
	for (size_t i = 1; i < link_num / 2; ++i)
		client.add_socket();
	client.do_something_to_all([port, &ip](test_client::object_ctype& item) {item->set_server_addr(port, ip);});

	//method #3, add clients and set server address in one invocation.
	for (auto i = std::max((size_t) 1, link_num / 2); i < link_num; ++i)
		client.add_socket(port, ip);

	size_t send_thread_num = 8;
	if (argc > 2)
		send_thread_num = (size_t) std::max(1, std::min(16, atoi(argv[2])));

	auto thread_num = 1;
	if (argc > 1)
		thread_num = std::min(16, std::max(thread_num, atoi(argv[1])));
	//add one thread will seriously impact IO throughput when doing performance benchmark, this is because the business logic is very simple (send original messages back,
	//or just add up total message size), under this scenario, just one service thread without receiving buffer will obtain the best IO throughput.
	//the server has such behavior too.

	sp.start_service(thread_num);
	while(sp.is_running())
	{
		std::string str;
		std::getline(std::cin, str);
		if (str.empty())
			;
		else if (LIST_STATUS == str)
		{
			printf("link #: " ST_ASIO_SF ", valid links: " ST_ASIO_SF ", invalid links: " ST_ASIO_SF "\n", client.size(), client.valid_size(), client.invalid_object_size());
			puts("");
			puts(client.get_statistic().to_string().data());
		}
		else if (LIST_ALL_CLIENT == str)
			client.list_all_object();
		else if (is_testing)
			puts("testing has not finished yet!");
		else if (QUIT_COMMAND == str)
			sp.stop_service();
		else if (RESTART_COMMAND == str)
		{
			sp.stop_service();
			sp.start_service(thread_num);
		}
		else
		{
			if ('+' == str[0] || '-' == str[0])
			{
				auto n = (size_t) atoi(std::next(str.data()));
				if (0 == n)
					n = 1;

				if ('+' == str[0])
					for (; n > 0 && client.add_socket(port, ip); --n);
				else
				{
					if (n > client.size())
						n = client.size();

					client.shutdown_some_client(n);
				}

				link_num = client.size();
				continue;
			}

#ifdef ST_ASIO_CLEAR_OBJECT_INTERVAL
			link_num = client.size();
			if (link_num != client.valid_size())
			{
				puts("please wait for a while, because st_object_pool has not cleaned up invalid links.");
				continue;
			}
#endif
			size_t msg_num = 1024;
			size_t msg_len = 1024; //must greater than or equal to sizeof(size_t)
			auto msg_fill = '0';
			char mode = 0; //0 broadcast, 1 randomly pick one link per msg
			auto repeat_times = 1;

			boost::char_separator<char> sep(" \t");
			boost::tokenizer<boost::char_separator<char>> parameters(str, sep);
			auto iter = std::begin(parameters);
			if (iter != std::end(parameters)) msg_num = std::max((size_t) atoll(iter++->data()), (size_t) 1);

#if 0 == PACKER_UNPACKER_TYPE || 1 == PACKER_UNPACKER_TYPE
			if (iter != std::end(parameters)) msg_len = std::min(packer::get_max_msg_size(),
				std::max((size_t) atoi(iter++->data()), sizeof(size_t))); //include seq
#elif 2 == PACKER_UNPACKER_TYPE
			if (iter != std::end(parameters)) ++iter;
			msg_len = 1024; //we hard code this because we fixedly initialized the length of fixed_length_unpacker to 1024
#elif 3 == PACKER_UNPACKER_TYPE
			if (iter != std::end(parameters)) msg_len = std::min((size_t) ST_ASIO_MSG_BUFFER_SIZE,
				std::max((size_t) atoi(iter++->data()), sizeof(size_t))); //include seq
#endif
			if (iter != std::end(parameters)) msg_fill = *iter++->data();
			if (iter != std::end(parameters)) mode = *iter++->data() - '0';
			if (iter != std::end(parameters)) repeat_times = std::max(atoi(iter++->data()), 1);

			if (0 != mode && 1 != mode)
				puts("unrecognized mode!");
			else
			{
				printf("test parameters after adjustment: " ST_ASIO_SF " " ST_ASIO_SF " %c %d\n", msg_num, msg_len, msg_fill, mode);
				puts("performance test begin, this application will have no response during the test!");

				is_testing = true;
				boost::thread(boost::bind(&start_test, repeat_times, mode, boost::ref(client), send_thread_num, msg_num, msg_len, msg_fill)).detach();
			}
		}
	}

    return 0;
}
