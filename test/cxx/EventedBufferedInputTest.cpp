#include <TestSupport.h>
#include <BackgroundEventLoop.h>
#include <EventedBufferedInput.h>
#include <Constants.h>
#include <Utils.h>
#include <Utils/IOUtils.h>
#include <Utils/StrIntUtils.h>

using namespace Passenger;
using namespace std;

namespace tut {
	class MyEventedBufferedInput: public EventedBufferedInput<> {
	public:
		boost::mutex syncher;
		int readError;
		function<void ()> onAfterProcessingBuffer;

		MyEventedBufferedInput(SafeLibev *libev, const FileDescriptor &fd)
			: EventedBufferedInput<>(libev, fd)
		{
			readError = 0;
		}

		virtual ssize_t readSocket(void *buf, size_t n) {
			int readError;
			{
				lock_guard<boost::mutex> l(syncher);
				readError = this->readError;
			}
			if (readError == 0) {
				return EventedBufferedInput<>::readSocket(buf, n);
			} else {
				errno = readError;
				return -1;
			}
		}

		void setReadError(int code) {
			lock_guard<boost::mutex> l(syncher);
			readError = code;
		}

		virtual void afterProcessingBuffer() {
			function<void ()> onAfterProcessingBuffer;
			{
				lock_guard<boost::mutex> l(syncher);
				onAfterProcessingBuffer = this->onAfterProcessingBuffer;
			}
			if (onAfterProcessingBuffer) {
				onAfterProcessingBuffer();
			}
		}
	};

	struct EventedBufferedInputTest {
		BackgroundEventLoop bg;
		Pipe p;
		shared_ptr<MyEventedBufferedInput> ebi;
		boost::mutex syncher;
		string log;
		ssize_t toConsume;
		unsigned int counter;
		
		EventedBufferedInputTest() {
			p = createPipe();
			ebi = make_shared<MyEventedBufferedInput>(bg.safe.get(), p.first);
			ebi->onData = onData;
			ebi->onError = onError;
			ebi->userData = this;
			toConsume = -1;
			counter = 0;
			bg.start();
		}

		~EventedBufferedInputTest() {
			bg.stop();
			setLogLevel(DEFAULT_LOG_LEVEL);
		}

		static size_t onData(const EventedBufferedInputPtr &input, const StaticString &data) {
			EventedBufferedInputTest *self = (EventedBufferedInputTest *) input->userData;
			lock_guard<boost::mutex> l(self->syncher);
			self->counter++;
			if (data.empty()) {
				self->log.append("EOF\n");
			} else {
				self->log.append("Data: " + cEscapeString(data) + "\n");
			}
			if (self->toConsume == -1) {
				return data.size();
			} else {
				return self->toConsume;
			}
		}

		static void onError(const EventedBufferedInputPtr &input, const char *message, int code) {
			EventedBufferedInputTest *self = (EventedBufferedInputTest *) input->userData;
			lock_guard<boost::mutex> l(self->syncher);
			self->log.append("Error: " + toString(code) + "\n");
		}

		void startEbi() {
			bg.safe->run(boost::bind(&EventedBufferedInputTest::realStartEbi, this));
		}

		void realStartEbi() {
			ebi->start();
		}

		bool ebiIsStarted() {
			bool result;
			bg.safe->run(boost::bind(&EventedBufferedInputTest::realEbiIsStarted, this, &result));
			return result;
		}

		void realEbiIsStarted(bool *result) {
			*result = ebi->isStarted();
		}

		void logEbiIsStarted() {
			lock_guard<boost::mutex> l(syncher);
			log.append("isStarted: " + toString(ebi->isStarted()) + "\n");
			log.append("isSocketStarted: " + toString(ebi->isSocketStarted()) + "\n");
		}
	};

	#define LOCK() lock_guard<boost::mutex> l(syncher)

	#define DEFINE_ON_DATA_METHOD(name, code) \
		static size_t name(const EventedBufferedInputPtr &input, const StaticString &data) { \
			EventedBufferedInputTest *self = (EventedBufferedInputTest *) input->userData; \
			boost::mutex &syncher = self->syncher; \
			(void) syncher; /* Shut up compiler warning */ \
			code \
		}
	
	DEFINE_TEST_GROUP(EventedBufferedInputTest);

	TEST_METHOD(1) {
		set_test_name("It emits socket data events upon receiving data");
		startEbi();
		writeExact(p.second, "aaabbb");
		EVENTUALLY(5,
			LOCK();
			result = !log.empty();
		);
		LOCK();
		ensure_equals(log, "Data: aaabbb\n");
	}

	TEST_METHOD(2) {
		set_test_name("It emits socket end events upon receiving EOF");
		startEbi();
		p.second.close();
		EVENTUALLY(5,
			LOCK();
			result = !log.empty();
		);
		LOCK();
		ensure_equals(log, "EOF\n");
	}

	TEST_METHOD(3) {
		set_test_name("It emits socket end events after all data has been consumed");
		startEbi();
		
		writeExact(p.second, "aaabbb");
		EVENTUALLY(5,
			LOCK();
			result = !log.empty();
		);
		{
			LOCK();
			ensure_equals(log, "Data: aaabbb\n");
		}

		p.second.close();
		EVENTUALLY(5,
			LOCK();
			result = log.find("EOF") != string::npos;
		);
		{
			LOCK();
			ensure_equals(log,
				"Data: aaabbb\n"
				"EOF\n");
		}
	}

	TEST_METHOD(4) {
		set_test_name("Considers ended sockets to be paused");
		startEbi();
		p.second.close();
		EVENTUALLY(5,
			LOCK();
			result = !log.empty();
		);
		ensure(!ebiIsStarted());
	}

	TEST_METHOD(5) {
		set_test_name("It emits error events upon encountering a socket error");
		startEbi();
		ebi->setReadError(EIO);
		writeExact(p.second, "aaabbb");
		EVENTUALLY(5,
			LOCK();
			result = !log.empty();
		);
		LOCK();
		ensure_equals(log, "Error: " + toString(EIO) + "\n");
	}

	TEST_METHOD(6) {
		set_test_name("It emits error events after all data has been consumed");
		startEbi();

		writeExact(p.second, "aaabbb");
		EVENTUALLY(5,
			LOCK();
			result = !log.empty();
		);

		ebi->setReadError(EIO);
		writeExact(p.second, "x");
		EVENTUALLY(5,
			LOCK();
			result = log.find("Error") != string::npos;
		);

		LOCK();
		ensure_equals(log,
			"Data: aaabbb\n"
			"Error: " + toString(EIO) + "\n");
	}

	TEST_METHOD(7) {
		set_test_name("Considers error'ed sockets to be paused");
		startEbi();
		ebi->setReadError(EIO);
		writeExact(p.second, "x");
		EVENTUALLY(5,
			LOCK();
			result = !log.empty();
		);
		LOCK();
		ensure(!ebiIsStarted());
	}

	DEFINE_ON_DATA_METHOD(on_data_8,
		input->stop();
		self->bg.safe->runLater(boost::bind(&EventedBufferedInputTest::logEbiIsStarted, self));
		return 3;
	)

	TEST_METHOD(8) {
		set_test_name("If the onData callback consumes everything and pauses the "
			"EventedBufferedInput, then the EventedBufferedInput leaves the socket "
			"in the paused state");
		
		ebi->onData = on_data_8;
		startEbi();
		writeExact(p.second, "abc");
		EVENTUALLY(5,
			LOCK();
			result = !log.empty();
		);
		LOCK();
		ensure_equals(log,
			"isStarted: 0\n"
			"isSocketStarted: 0\n");
	}

	DEFINE_ON_DATA_METHOD(on_data_9,
		input->start();
		self->bg.safe->runLater(boost::bind(&EventedBufferedInputTest::logEbiIsStarted, self));
		return 3;
	)

	TEST_METHOD(9) {
		set_test_name("if the onData callback consumes everything and resumes the "
			"EventedBufferedInput, then the EventedBufferedInput leaves the socket "
			"in the resumed state");

		ebi->onData = on_data_9;
		startEbi();
		writeExact(p.second, "abc");
		EVENTUALLY(5,
			LOCK();
			result = !log.empty();
		);
		LOCK();
		ensure_equals(log,
			"isStarted: 1\n"
			"isSocketStarted: 1\n");
	}

	DEFINE_ON_DATA_METHOD(on_data_10,
		input->stop();
		self->bg.safe->runLater(boost::bind(&EventedBufferedInputTest::logEbiIsStarted, self));
		return 1;
	)

	TEST_METHOD(10) {
		set_test_name("If the onData callback consumes partially and pauses the "
			"EventedBufferedInput, then the EventedBufferedInput leaves the socket "
			"at the paused state");

		ebi->onData = on_data_10;
		startEbi();
		writeExact(p.second, "abc");
		EVENTUALLY(5,
			LOCK();
			result = !log.empty();
		);
		LOCK();
		ensure_equals(log,
			"isStarted: 0\n"
			"isSocketStarted: 0\n");
	}

	DEFINE_ON_DATA_METHOD(on_data_11,
		input->start();
		self->bg.safe->runLater(boost::bind(&EventedBufferedInputTest::logEbiIsStarted, self));
		return 1;
	)

	TEST_METHOD(11) {
		set_test_name("If the onData callback consumes partially and resumes the "
			"EventedBufferedInput, then the EventedBufferedInput leaves the socket "
			"at the resumed state");

		ebi->onData = on_data_11;
		startEbi();
		writeExact(p.second, "ab");
		EVENTUALLY(5,
			LOCK();
			result = !log.empty();
		);
		LOCK();
		ensure_equals(log,
			"isStarted: 1\n"
			"isSocketStarted: 0\n"
			"isStarted: 1\n"
			"isSocketStarted: 1\n");
	}

	DEFINE_ON_DATA_METHOD(on_data_12,
		LOCK();
		self->counter++;
		if (self->counter == 2) {
			input->stop();
			self->bg.safe->runLater(boost::bind(&EventedBufferedInputTest::logEbiIsStarted, self));
		}
		return 2;
	)

	TEST_METHOD(12) {
		set_test_name("If the onData callback first consumes partially, then "
			"consumes everything and pauses the EventedBufferedInput, then the "
			"EventedBufferedInput leaves the socket in the paused state");

		ebi->onData = on_data_12;
		startEbi();
		writeExact(p.second, "aabb");
		EVENTUALLY(5,
			LOCK();
			result = !log.empty();
		);
		LOCK();
		ensure_equals(log,
			"isStarted: 0\n"
			"isSocketStarted: 0\n");
	}

	DEFINE_ON_DATA_METHOD(on_data_13,
		LOCK();
		self->counter++;
		if (self->counter == 2) {
			input->start();
			self->bg.safe->runLater(boost::bind(&EventedBufferedInputTest::logEbiIsStarted, self));
		}
		return 2;
	)

	TEST_METHOD(13) {
		set_test_name("If the onData callback first consumes partially, then "
			"consumes everything and resumes the EventedBufferedInput, then the "
			"EventedBufferedInput leaves the socket in the resumed state");

		ebi->onData = on_data_13;
		startEbi();
		writeExact(p.second, "aabb");
		EVENTUALLY(5,
			LOCK();
			result = !log.empty();
		);
		LOCK();
		ensure_equals(log,
			"isStarted: 1\n"
			"isSocketStarted: 1\n");
	}


	/*** If the onData callback didn't consume everything... ***/

		DEFINE_ON_DATA_METHOD(on_data_20,
			LOCK();
			self->counter++;
			self->log.append("onData called; isSocketStarted: " +
				toString(self->ebi->isSocketStarted()) + "\n");
			self->log.append("Data: " + cEscapeString(data) + "\n");
			if (self->counter == 1) {
				return 3;
			} else {
				return 1;
			}
		)

		static void on_after_processing_buffer_20(EventedBufferedInputTest *self) {
			lock_guard<boost::mutex> l(self->syncher);
			if (self->counter == 1) {
				self->log.append("Finished first onData; isSocketStarted: " +
					toString(self->ebi->isSocketStarted()) + "\n");
			}
		}

		static void finish_20(EventedBufferedInputTest *self) {
			lock_guard<boost::mutex> l(self->syncher);
			self->log.append("Finished; isSocketStarted: " +
				toString(self->ebi->isStarted()) + "\n");
		}

		TEST_METHOD(20) {
			set_test_name("It pauses the socket, re-emits the remaining data in the next tick, "
				"then resumes the socket when everything is consumed");

			ebi->onData = on_data_20;
			ebi->onAfterProcessingBuffer = boost::bind(on_after_processing_buffer_20, this);
			startEbi();
			writeExact(p.second, "aaabbb");
			bg.safe->runAfter(10, boost::bind(finish_20, this));

			EVENTUALLY(5,
				LOCK();
				result = log.find("Finished;") != string::npos;
			);
			LOCK();
			ensure_equals(log,
				"onData called; isSocketStarted: 1\n"
				"Data: aaabbb\n"
				"Finished first onData; isSocketStarted: 0\n"
				"onData called; isSocketStarted: 0\n"
				"Data: bbb\n"
				"onData called; isSocketStarted: 0\n"
				"Data: bb\n"
				"onData called; isSocketStarted: 0\n"
				"Data: b\n"
				"Finished; isSocketStarted: 1\n");
		}

		/*** If pause() is called after the data handler... ***/

			static void on_after_processing_buffer_21(EventedBufferedInputTest *self) {
				self->ebi->stop();
				lock_guard<boost::mutex> l(self->syncher);
				self->log.append("isSocketStarted: " +
					toString(self->ebi->isStarted()) + "\n");
			}

			TEST_METHOD(21) {
				set_test_name("It pauses the socket and doesn't re-emit remaining data events");
				toConsume = 1;
				ebi->onAfterProcessingBuffer = boost::bind(on_after_processing_buffer_21, this);
				startEbi();
				writeExact(p.second, "aaabbb");
				EVENTUALLY(5,
					LOCK();
					result = log.find("isSocketStarted") != string::npos;
				);
				LOCK();
				ensure_equals(log,
					"Data: aaabbb\n"
					"isSocketStarted: 0\n");
			}

			TEST_METHOD(22) {
				set_test_name("Resumes the socket and re-emits remaining data one tick after resume() is called");
			}

			TEST_METHOD(23) {
				set_test_name("Considers error'ed sockets to be paused");
			}

			TEST_METHOD(24) {
				set_test_name("Considers error'ed sockets to be paused");
			}
}
