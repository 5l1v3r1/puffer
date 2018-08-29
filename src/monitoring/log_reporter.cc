#include <cstdlib>
#include <fcntl.h>

#include <iostream>
#include <vector>
#include <string>
#include <fstream>

#include "util.hh"
#include "inotify.hh"
#include "poller.hh"
#include "file_descriptor.hh"
#include "tokenize.hh"
#include "exception.hh"
#include "system_runner.hh"
#include "socket.hh"
#include "http_request.hh"

using namespace std;
using namespace PollerShortNames;

void print_usage(const string & program_name)
{
  cerr << program_name << " <log path> <log config>" << endl;
}

void post_to_db(TCPSocket & db_sock, const vector<string> & data,
                const string & buf)
{
  vector<string> line = split(buf, " ");

  string data_str;
  for (const auto & e : data) {
    if (e.front() == '{' and e.back() == '}') {
      /* fill in with value from corresponding column */
      unsigned int column_idx = stoi(e.substr(1)) - 1;
      if (column_idx >= line.size()) {
        cerr << "Silent error: invalid column " << column_idx + 1 << endl;
        return;
      }

      data_str += line.at(column_idx);
    } else {
      data_str += e;
    }
  }

  /* send POST request to InfluxDB */
  HTTPRequest request;
  request.set_first_line("POST /write?db=collectd&u=puffer&p="
      + safe_getenv("INFLUXDB_PASSWORD") + "&precision=s HTTP/1.1");
  request.add_header(HTTPHeader{"Host", "localhost:8086"});
  request.add_header(HTTPHeader{"Accept", "*/*"});
  request.add_header(HTTPHeader{"Content-Type", "application/x-www-form-urlencoded"});
  request.add_header(HTTPHeader{"Content-Length", to_string(data_str.length())});
  request.done_with_headers();
  request.read_in_body(data_str);

  db_sock.write(move(request.str()));
}

int tail_loop(const string & log_path, TCPSocket & db_sock,
              const vector<string> & data)
{
  bool log_rotated = false;  /* whether log rotation happened */
  string buf;  /* buffer to assemble lines read from the log */
  vector<string> lines;  /* lines waiting to be posted to InfluxDB */

  Poller poller;

  poller.add_action(Poller::Action(db_sock, Direction::In,
    [&db_sock]()->Result {
      /* read but ignore HTTP responses from InfluxDB */
      const string response = db_sock.read();
      if (response.empty()) {
        throw runtime_error("Peer socket in InfluxDB has closed");
      }

      return ResultType::Continue;
    }
  ));

  poller.add_action(Poller::Action(db_sock, Direction::Out,
    [&db_sock, &lines, &data]()->Result {
      /* post each line to InfluxDB */
      for (const string & line : lines) {
        post_to_db(db_sock, data, line);
      }
      lines.clear();

      return ResultType::Continue;
    },
    [&lines]()->bool {
      return not lines.empty();
    }
  ));

  Inotify inotify(poller);

  for (;;) {
    /* read new lines from the end */
    FileDescriptor fd(CheckSystemCall("open (" + log_path + ")",
                                      open(log_path.c_str(), O_RDONLY)));
    fd.seek(0, SEEK_END);

    int wd = inotify.add_watch(log_path, IN_MODIFY | IN_CLOSE_WRITE,
      [&log_rotated, &buf, &fd, &db_sock, &data, &lines]
      (const inotify_event & event, const string &) {
        if (event.mask & IN_MODIFY) {
          for (;;) {
            string new_content = fd.read();
            if (new_content.empty()) {
              /* break if nothing more to read */
              break;
            }

            /* find new lines iteratively */
            for (;;) {
              auto pos = new_content.find("\n");
              if (pos == string::npos) {
                buf += new_content;
                new_content = "";
                break;
              } else {
                buf += new_content.substr(0, pos);
                /* buf is a complete line now */
                lines.emplace_back(move(buf));

                buf = "";
                new_content = new_content.substr(pos + 1);
              }
            }
          }
        } else if (event.mask & IN_CLOSE_WRITE) {
          /* old log has been closed; open recreated log in next loop */
          log_rotated = true;
        }
      }
    );

    while (not log_rotated) {
      auto ret = poller.poll(-1);
      if (ret.result != Poller::Result::Type::Success) {
        return ret.exit_status;
      }
    }

    inotify.rm_watch(wd);
    log_rotated = false;
  }

  return EXIT_SUCCESS;
}

int main(int argc, char * argv[])
{
  if (argc < 1) {
    abort();
  }

  if (argc != 3) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  /* create an empty log if it does not exist */
  string log_path = argv[1];
  FileDescriptor touch(CheckSystemCall("open (" + log_path + ")",
                       open(log_path.c_str(), O_WRONLY | O_CREAT, 0644)));
  touch.close();

  /* create socket connected to influxdb */
  TCPSocket db_sock;
  Address influxdb_addr("127.0.0.1", 8086);
  db_sock.connect(influxdb_addr);

  /* read a line from the config file */
  ifstream config_file(argv[2]);
  string config_line;
  getline(config_file, config_line);

  /* data for "--data-binary": store a "format string" in a vector */
  vector<string> data;

  size_t pos = 0;
  while (pos < config_line.size()) {
    size_t left_pos = config_line.find("{", pos);
    if (left_pos == string::npos) {
      data.emplace_back(config_line.substr(pos, config_line.size() - pos));
      break;
    }

    if (left_pos - pos > 0) {
      data.emplace_back(config_line.substr(pos, left_pos - pos));
    }
    pos = left_pos + 1;

    size_t right_pos = config_line.find("}", pos);
    if (right_pos == string::npos) {
      cerr << "Wrong config format: no matching } for {" << endl;
      return EXIT_FAILURE;
    } else if (right_pos - left_pos == 1) {
      cerr << "Error: empty column number between { and }" << endl;
      return EXIT_FAILURE;
    }

    const auto & column_no = config_line.substr(left_pos + 1,
                                                right_pos - left_pos - 1);
    if (stoi(column_no) <= 0) {
      cerr << "Error: invalid column number between { and }" << endl;
      return EXIT_FAILURE;
    }

    data.emplace_back("{" + column_no + "}");
    pos = right_pos + 1;
  }

  /* read new lines from log and post to InfluxDB */
  return tail_loop(log_path, db_sock, data);
}
