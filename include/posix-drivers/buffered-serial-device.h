/*
 * This file is part of the µOS++ distribution.
 *   (https://github.com/micro-os-plus)
 * Copyright (c) 2015 Liviu Ionescu.
 *
 * µOS++ is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, version 3.
 *
 * µOS++ is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef POSIX_DRIVERS_BUFFERED_SERIAL_DEVICE_H_
#define POSIX_DRIVERS_BUFFERED_SERIAL_DEVICE_H_

// ----------------------------------------------------------------------------

#include "posix-io/CharDevice.h"
#include "posix-drivers/ByteCircularBuffer.h"
#include "posix-drivers/cmsis-driver-serial.h"

#include "diag/trace.h"

#include "cmsis_os.h"
#include "Driver_USART.h"

#include <cstdarg>
#include <cstdlib>
#include <cerrno>
#include <cassert>

// ----------------------------------------------------------------------------

// TODO: (multiline)
// - add flow control on both send & receive
// - add link control (connected/disconnected)
// - cancel pending reads/writes at close
// - add error processing

namespace os
{
  namespace dev
  {
    // ------------------------------------------------------------------------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpadded"

    template<typename Cs_T>
      class Buffered_serial_device : public os::posix::CharDevice
      {
        using Critical_section = Cs_T;

      public:

        Buffered_serial_device (const char* device_name,
                                os::cmsis::driver::Serial* driver,
                                os::dev::ByteCircularBuffer* rx_buf,
                                os::dev::ByteCircularBuffer* tx_buf);

        virtual
        ~Buffered_serial_device ();

        // --------------------------------------------------------------------

        // Static callback, that will call the object callback
        static void
        signal_event (Buffered_serial_device* object, uint32_t event);

        void
        do_signal_event (uint32_t event);

        // --------------------------------------------------------------------

      protected:

        virtual int
        do_vopen (const char* path, int oflag, std::va_list args) override;

        virtual int
        do_close (void) override;

        virtual ssize_t
        do_read (void* buf, std::size_t nbyte) override;

        virtual ssize_t
        do_write (const void* buf, std::size_t nbyte) override;

#if 0
        virtual ssize_t
        do_writev (const struct iovec* iov, int iovcnt) override;

        virtual int
        do_vioctl (int request, std::va_list args) override;

        virtual int
        do_vfcntl (int cmd, va_list args) override;
#endif

        virtual bool
        doIsOpened (void) override;

        // --------------------------------------------------------------------

      private:

        // Pointer to actual CMSIS-like serial driver (usart or usb cdc acm)
        os::cmsis::driver::Serial* driver_;

        osSemaphoreId rx_sem_; //
        osSemaphoreDef(rx_sem_);

        osSemaphoreId tx_sem_; //
        osSemaphoreDef(tx_sem_);

        os::dev::ByteCircularBuffer* rx_buf_;
        os::dev::ByteCircularBuffer* tx_buf_;

        std::size_t rx_count_; //
        bool volatile tx_busy_;
        // Padding!

      };

#pragma GCC diagnostic pop

    // ------------------------------------------------------------------------

    template<typename Cs_T>
      Buffered_serial_device<Cs_T>::Buffered_serial_device (
          const char* deviceName, //
          os::cmsis::driver::Serial* driver,
          os::dev::ByteCircularBuffer* rx_buf,
          os::dev::ByteCircularBuffer* tx_buf) :
          //
          CharDevice (deviceName), // Construct parent.
          //
          driver_ (driver), //
          rx_sem_ (nullptr), //
          tx_sem_ (nullptr), //
          rx_buf_ (rx_buf), //
          tx_buf_ (tx_buf), //
          rx_count_ (0), //
          tx_busy_ (false) //
      {
        assert(rx_buf != nullptr);
        // tx_buf may be null.
      }

    template<typename Cs_T>
      Buffered_serial_device<Cs_T>::~Buffered_serial_device ()
      {
        rx_sem_ = nullptr;
        tx_sem_ = nullptr;
      }

    // ------------------------------------------------------------------------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

    template<typename Cs_T>
      int
      Buffered_serial_device<Cs_T>::do_vopen (const char* path, int oflag,
                                              std::va_list args)
      {
        if (rx_sem_ != nullptr)
          {
            errno = EEXIST; // Already opened
            return -1;
          }

        int32_t result;

        do
          {
            // Start disabled, the first wait will block.
            rx_sem_ = osSemaphoreCreate (osSemaphore(rx_sem_), 0);
            tx_sem_ = osSemaphoreCreate (osSemaphore(tx_sem_), 0);

            if ((rx_sem_ == nullptr) || (tx_sem_ == nullptr))
              {
                result = ARM_DRIVER_ERROR;
                break;
              }

            // Clear buffers.
            rx_buf_->clear ();
            if (tx_buf_ != nullptr)
              {
                tx_buf_->clear ();
              }

            // Initialise the driver to call back this instance.
            result =
                driver_->initialize (
                    reinterpret_cast<os::cmsis::driver::Serial::signal_event_t> (signal_event),
                    this);
            if (result != ARM_DRIVER_OK)
              break;

            result = driver_->power (ARM_POWER_FULL);
            if (result != ARM_DRIVER_OK)
              break;

            // Default configuration: 8 bits, no parity, 1 stop bit,
            // no flow control, 115200 bps.
            result = driver_->control (ARM_USART_MODE_ASYNCHRONOUS |
            ARM_USART_DATA_BITS_8 |
            ARM_USART_PARITY_NONE |
            ARM_USART_STOP_BITS_1 |
            ARM_USART_FLOW_CONTROL_NONE,
                                       115200);
            if (result != ARM_DRIVER_OK)
              break;

            // Enable TX output.
            result = driver_->control (ARM_USART_CONTROL_TX, 1);
            if (result != ARM_DRIVER_OK)
              break;

            // Enable RX input.
            result = driver_->control (ARM_USART_CONTROL_RX, 1);
            if (result != ARM_DRIVER_OK)
              break;

            uint8_t* pbuf;
            std::size_t nbyte = rx_buf_->getBackContiguousBuffer (&pbuf);

            result = driver_->receive (pbuf, nbyte);
            if (result != ARM_DRIVER_OK)
              break;
          }
        while (false); // Actually NOT a loop, just a sequence of ifs!

        if (result != ARM_DRIVER_OK)
          {
            driver_->power (ARM_POWER_OFF);
            driver_->uninitialize ();

            errno = ENOSR;
            return -1;
          }

        // Return POSIX OK.
        return 0;
      }

    template<typename Cs_T>
      bool
      Buffered_serial_device<Cs_T>::doIsOpened (void)
      {
        return (rx_sem_ != nullptr);
      }

    template<typename Cs_T>
      int
      Buffered_serial_device<Cs_T>::do_close (void)
      {
        osSemaphoreDelete (rx_sem_);
        rx_sem_ = nullptr;

        osSemaphoreDelete (tx_sem_);
        tx_sem_ = nullptr;

        // Disable USART and I/O pins used.
        driver_->control (ARM_USART_CONTROL_TX, 0);
        driver_->control (ARM_USART_CONTROL_RX, 0);
        driver_->power (ARM_POWER_OFF);
        driver_->uninitialize ();

        // Return POSIX OK.
        return 0;
      }

    template<typename Cs_T>
      ssize_t
      Buffered_serial_device<Cs_T>::do_read (void* buf, std::size_t nbyte)
      {
        // TODO: implement cases when 0 must be returned
        // (disconnects, timeouts).
        while (true)
          {
            std::size_t count;
              {
                Critical_section cs; // -----

                count = rx_buf_->popFront (static_cast<uint8_t*> (buf), nbyte);
              }
            if (count > 0)
              {
                // Actual number of chars received in buffer.
                return count;
              }

            // Block and wait for bytes to arrive.
            osSemaphoreWait (rx_sem_, osWaitForever);
          }
      }

    template<typename Cs_T>
      ssize_t
      Buffered_serial_device<Cs_T>::do_write (const void* buf,
                                              std::size_t nbyte)
      {
        std::size_t count;

        if (tx_buf_ != nullptr)
          {
            count = 0;
              {
                Critical_section cs; // -----

                if (tx_buf_->isBelowHighWaterMark ())
                  {
                    // If there is more space in the buffer, try to fill it.
                    count = tx_buf_->pushBack (
                        static_cast<const uint8_t*> (buf), nbyte);
                  }
              }
            while (true)
              {
//                ARM_USART_STATUS status;
//                  {
//                    Critical_section cs; // -----
//
//#pragma GCC diagnostic push
//#pragma GCC diagnostic ignored "-Waggregate-return"
//                    status = driver_->get_status ();
//#pragma GCC diagnostic pop
//
//                  }
                // We use a local tx busy flag because the ARM driver's flag
                // may become not-busy between transmissions.
                if (!tx_busy_)
                  {
                    uint8_t* pbuf;
                    std::size_t nb;
                      {
                        Critical_section cs; // -----

                        nb = tx_buf_->getFrontContiguousBuffer (&pbuf);
                      }
                    if (nb > 0)
                      {
                        if (driver_->send (pbuf, nb) != ARM_DRIVER_OK)
                          {
                            errno = EIO;
                            return -1;
                          }
                        tx_busy_ = true;
                      }
                  }

//                bool isBelowHWM;
//                  {
//                    Critical_section cs; // -----
//
//                    isBelowHWM = tx_buf_->isBelowHighWaterMark ();
//                  }
                if (count == nbyte)
                  {
                    return nbyte;
                  }

                // Block and wait for buffer to be freed.
                osSemaphoreWait (tx_sem_, osWaitForever);

                if (count < nbyte)
                  {
                    Critical_section cs;  // -----

                    std::size_t n;
                    // If there is more space in the buffer, try to fill it.
                    n = tx_buf_->pushBack (
                        static_cast<const uint8_t*> (buf) + count,
                        nbyte - count);
                    count += n;
                  }
              }
          }
        else
          {
            // Do not use a transmit buffer, send directly from the user buffer.
            // Wait while transmitting.
            ARM_USART_STATUS status;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waggregate-return"
            status = driver_->get_status ();
#pragma GCC diagnostic pop
            if (status.tx_busy)
              {
                osSemaphoreWait (tx_sem_, osWaitForever);
              }

            if ((driver_->send (buf, nbyte)) == ARM_DRIVER_OK)
              {
                osSemaphoreWait (tx_sem_, osWaitForever);
                count = driver_->get_tx_count ();
              }
            else
              {
                count = -1;
                errno = EIO;
              }
          }
        // Actual number of bytes transmitted from buffer.
        return count;
      }

#if 0
    template<typename Cs_T>
    ssize_t
    Buffered_serial_device<Cs_T>::do_writev (const struct iovec* iov, int iovcnt)
      {
        errno = ENOSYS; // Not implemented
        return -1;
      }

    template<typename Cs_T>
    int
    Buffered_serial_device<Cs_T>::do_vioctl (int request, std::va_list args)
      {
        errno = ENOSYS; // Not implemented
        return -1;
      }

    template<typename Cs_T>
    int
    Buffered_serial_device<Cs_T>::do_vfcntl (int cmd, std::va_list args)
      {
        errno = ENOSYS; // Not implemented
        return -1;
      }
#endif

    // ------------------------------------------------------------------------

    // This function is called by the CMSIS driver in an interrupt context.

    template<typename Cs_T>
      void
      Buffered_serial_device<Cs_T>::do_signal_event (uint32_t event)
      {
        if ((event
            & (ARM_USART_EVENT_RECEIVE_COMPLETE
                | ARM_USART_EVENT_RX_FRAMING_ERROR | ARM_USART_EVENT_RX_TIMEOUT)))
          {
            // TODO: process errors and timeouts
            std::size_t tmpCount = driver_->get_rx_count ();
            std::size_t count = tmpCount - rx_count_;
            rx_count_ = tmpCount;
            std::size_t adjust = rx_buf_->advanceBack (count);
            assert(count == adjust);

            if (event & ARM_USART_EVENT_RECEIVE_COMPLETE)
              {
                uint8_t* pbuf;
                std::size_t nbyte = rx_buf_->getBackContiguousBuffer (&pbuf);
                if (nbyte == 0)
                  {
                    // Overwrite the last byte, but keep the driver in
                    // receive mode continuously.
                    rx_buf_->retreatBack ();
                    nbyte = rx_buf_->getBackContiguousBuffer (&pbuf);
                  }
                assert(nbyte > 0);

                // Read as much as we can.
                int32_t status;
                status = driver_->receive (pbuf, nbyte);
                // TODO: implement error processing.
                assert(status == ARM_DRIVER_OK);

                rx_count_ = 0;
              }
            if (count > 0)
              {
                // Immediately wake up, do not wait to reach any water mark.
                osSemaphoreRelease (rx_sem_);
              }
          }
        if (event & ARM_USART_EVENT_TX_COMPLETE)
          {
            if (tx_buf_ != nullptr)
              {
                std::size_t count = driver_->get_tx_count ();
                std::size_t adjust = tx_buf_->advanceFront (count);
                assert(count == adjust);

                uint8_t* pbuf;
                std::size_t nbyte = tx_buf_->getFrontContiguousBuffer (&pbuf);
                if (nbyte > 0)
                  {
                    int32_t status;
                    status = driver_->send (pbuf, nbyte);
                    // TODO: implement error processing
                    assert(status == ARM_DRIVER_OK);
                  }
                else
                  {
                    tx_busy_ = false;
                  }
                if (tx_buf_->isBelowLowWaterMark ())
                  {
                    // Wake up thread, to come and send more bytes.
                    osSemaphoreRelease (tx_sem_);
                  }
              }
            else
              {
                // No buffer, wake up the thread to return from write().
                osSemaphoreRelease (tx_sem_);
              }
          }
      }

    // Static call-back; forward to object implementation.
    template<typename Cs_T>
      void
      Buffered_serial_device<Cs_T>::signal_event (
          Buffered_serial_device* object, uint32_t event)
      {
        object->do_signal_event (event);
      }

#pragma GCC diagnostic pop

  } /* namespace dev */
} /* namespace os */

#endif /* POSIX_DRIVERS_BUFFERED_SERIAL_DEVICE_H_ */