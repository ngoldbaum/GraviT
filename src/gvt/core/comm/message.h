/* =======================================================================================
   This file is released as part of GraviT - scalable, platform independent ray tracing
   tacc.github.io/GraviT

   Copyright 2013-2015 Texas Advanced Computing Center, The University of Texas at Austin
   All rights reserved.

   Licensed under the BSD 3-Clause License, (the "License"); you may not use this file
   except in compliance with the License.
   A copy of the License is included with this software in the file LICENSE.
   If your copy does not contain the License, you may obtain a copy of the License at:

       http://opensource.org/licenses/BSD-3-Clause

   Unless required by applicable law or agreed to in writing, software distributed under
   the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
   KIND, either express or implied.
   See the License for the specific language governing permissions and limitations under
   limitations under the License.

   GraviT is funded in part by the US National Science Foundation under awards ACI-1339863,
   ACI-1339881 and ACI-1339840
   ======================================================================================= */
#ifndef GVT_CORE_MESSAGE_H
#define GVT_CORE_MESSAGE_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

/**
 *  \brief Communication message definiton
 *
 */

namespace gvt {
namespace comm {
/**
 *  \brief High level message tagging
 */
enum SYSTEM_COMM_TAG {
  CONTROL_SYSTEM_TAG = 0x8 /**< Used internally by the raytracing framework*/,
  CONTROL_USER_TAG /**< Developer level message */,
  CONTROL_VOTE_TAG /**< Voting message */
};

/**
 * Register a message type in the communicator
 * @method REGISTERABLE_MESSAGE
 * @param  ClassName            Name of the class to register
 * @return                      Return the message type identifer in the communicator
 */
#define REGISTERABLE_MESSAGE(ClassName) static int COMMUNICATOR_MESSAGE_TAG;
/**
 * Register a message type in the communicator
 * @method REGISTERABLE_MESSAGE
 * @param  ClassName            Name of the class to register
 * @return                      Return the message type identifer in the communicator
 */
#define REGISTER_INIT_MESSAGE(ClassName) int ClassName::COMMUNICATOR_MESSAGE_TAG = -1;

/**
 * \brief Abstract Communication Message
 */
struct Message {

  typedef unsigned char Byte;

  REGISTERABLE_MESSAGE(Message);

  /**
   * \brief Message header definition
   */
  struct header {
    std::size_t USER_TAG;                      /**< Message identifer at user level */
    std::size_t SYSTEM_TAG = CONTROL_USER_TAG; /**< System tag indentifier (By default always at user level) */
    std::size_t USER_MSG_SIZE;                 /**< Size of the buffer to be sent as defined by the user */
    long dst;                                  /**< Compute node id destination */
    long src;                                  /**< Compute node ID origin */
  };

  /**
   * \brief Create a message with buffer size
   * @param size Size of the buffer in bytes
   */
  Message(const std::size_t &size = 0);

  /**
   * \brief Copy constructor
   */
  Message(const Message &msg);
  /**
   * \brief Move semantics constructor
   */
  Message(Message &&msg);

  ~Message();

  /**
   * Get the header of the message
   * @method getHeader
   * @return reference to header object
   */
  header &getHeader();

  /**
   * Return the message tag/type
   * @method tag
   * @return Message tag
   */
  std::size_t tag();
  /**
   * Sets the message tag
   * @method tag
   * @param  tag identifier @see SYSTEM_COMM_TAG
   */
  void tag(const std::size_t tag);
  /**
   * Get message size
   * @method size
   * @return return buffer size
   */
  std::size_t size();


  /**
   * Set buffer size
   * @param size Size of the buffer to send in bytes
   */
  void size(const std::size_t size);

  /**
   * Return number of elements of type T
   * @return number of elements of type T
   */
  template <typename T> std::size_t sizehas() { return getHeader().USER_MSG_SIZE / sizeof(T); };

  /**
   * Return system type
   * @return @see SYSTEM_COMM_TAG
   */
  std::size_t system_tag();
  /**
   * Set system tag
   * @param std::size_t @see SYSTEM_COMM_TAG
   */
  void system_tag(std::size_t);

  /**
   * Return header+_buffer size
   * @return buffer size in bytes
   */
  std::size_t buffer_size() const { return _buffer_size; };

  /**
   * Message compute node destination
   * @return compute node id
   */
  long &dst() { return getHeader().dst; }
  /**
   * Message compute node source
   * @return compute node id
   */
  long &src() { return getHeader().src; }

  /**
   * Set message destination cpompute node
   * @param d destination compute node id
   */
  void dst(long d) { getHeader().dst = d; }
  /**
   * Ser message source compute node
   * @param s source compute node id
   */
  void src(long s) { getHeader().src = s; }

  /**
   * Returns the message content as a buffer of type T
   * @return pointer to buffer of type T
   */
  template <typename T> T *getMessage() { return reinterpret_cast<T *>(content); }
  /**
   * Set message content
   * @param orig Original buffer of type T
   * @param os   Number of elements of type T in the buffer
   */
  template <typename T> void setMessage(T *orig, const std::size_t &os) {
    header &mhi = getHeader();
    std::size_t bs = sizeof(T) * os;
    if (_buffer_size > 0) {
      content = static_cast<Byte *>(std::realloc(content, bs + sizeof(header)));
    } else {
      content = static_cast<Byte *>(std::malloc(bs + sizeof(header)));
    }
    _buffer_size = bs + sizeof(header);
    std::memcpy(content, orig, bs);
    std::memcpy(content + bs, &mhi, sizeof(header));
    size(bs);
  }

protected:
  std::size_t _buffer_size = 0;
  Byte *content = nullptr;
};

/**
 * @brief Empty message instanciation
 */
struct EmptyMessage : public gvt::comm::Message {
  REGISTERABLE_MESSAGE(EmptyMessage);

protected:
public:
  EmptyMessage() : gvt::comm::Message() { tag(COMMUNICATOR_MESSAGE_TAG); };
  EmptyMessage(const size_t &n) : gvt::comm::Message(n) { tag(COMMUNICATOR_MESSAGE_TAG); };
};
}
}

#endif
